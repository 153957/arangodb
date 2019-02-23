////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "Aql/SingleRemoteModificationExecutor.h"

#include "Aql/AqlValue.h"
#include "Aql/Collection.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/SingleRowFetcher.h"
#include "Basics/Common.h"
#include "ModificationExecutorTraits.h"
#include "VocBase/LogicalCollection.h"

#include <algorithm>

using namespace arangodb;
using namespace arangodb::aql;

template <typename Modifier>
SingleRemoteModificationExecutor<Modifier>::SingleRemoteModificationExecutor(Fetcher& fetcher,
                                                                             Infos& info, SingleRemoteModificationExecutorInfo& remoteInfo)
    : _info(info), _remoteInfo(remoteInfo), _fetcher(fetcher), _upstreamState(ExecutionState::HASMORE){};

template <typename Modifier>
SingleRemoteModificationExecutor<Modifier>::~SingleRemoteModificationExecutor() = default;

template <typename Modifier>
std::pair<ExecutionState, typename SingleRemoteModificationExecutor<Modifier>::Stats>
SingleRemoteModificationExecutor<Modifier>::produceRow(OutputAqlItemRow& output) {
  Stats stats;
  InputAqlItemRow input = InputAqlItemRow(CreateInvalidInputRowHint{});

  if (_upstreamState == ExecutionState::DONE) {
    return {_upstreamState, std::move(stats)};
  }

  std::tie(_upstreamState, input) = _fetcher.fetchRow();

  if (input.isInitialized()) {
    TRI_ASSERT(_upstreamState == ExecutionState::HASMORE ||
               _upstreamState == ExecutionState::DONE);
    doSingleRemoteModificationOperation(input, output, stats);
  } else {
    TRI_ASSERT(_upstreamState == ExecutionState::WAITING ||
               _upstreamState == ExecutionState::DONE);
  }
  return {_upstreamState, std::move(stats)};
}

template <typename Modifier>
bool SingleRemoteModificationExecutor<Modifier>::doSingleRemoteModificationOperation(
    InputAqlItemRow& input, OutputAqlItemRow& output, Stats& stats) {
  const bool isIndex = std::is_same<Modifier, Index>::value;
  const bool isInsert = std::is_same<Modifier, Insert>::value;
  const bool isRemove = std::is_same<Modifier, Remove>::value;
  const bool isUpdate = std::is_same<Modifier, Update>::value;
  const bool isReplace = std::is_same<Modifier, Replace>::value;

  const bool replaceIndex = false;

  int possibleWrites = 0;  // TODO - get real statistic values!

  OperationOptions& options = _info._options;

  if (_key.empty() && !_info._input1RegisterId.has_value()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND,
                                   "missing document reference");
  }

  VPackBuilder inBuilder;
  VPackSlice inSlice = VPackSlice::emptyObjectSlice();
  if (_info._input1RegisterId.has_value()) {  // IF NOT REMOVE OR SELECT
    // if (_buffer.size() < 1) {
    //  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND,
    //                                 "missing document reference in Register");
    //}
    AqlValue const& inDocument = input.getValue(_info._input1RegisterId.value());
    inBuilder.add(inDocument.slice());
    inSlice = inBuilder.slice();
  }

  std::unique_ptr<VPackBuilder> mergedBuilder;
  if (!_key.empty()) {
    // FIXME mergedBuilder = merge(inSlice, _key, 0);
    inSlice = mergedBuilder->slice();
  }

  OperationResult result;
  if (isIndex) {
    result = _info._trx->document(_info._aqlCollection->name(), inSlice, _info._options);
  } else if (isInsert) {
    if (options.returnOld && !options.overwrite) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_QUERY_VARIABLE_NAME_UNKNOWN,
          "OLD is only available when using INSERT with the overwrite option");
    }
    result = _info._trx->insert(_info._aqlCollection->name(), inSlice, _info._options);
    possibleWrites = 1;
  } else if (isRemove) {
    result = _info._trx->remove(_info._aqlCollection->name(), inSlice, _info._options);
    possibleWrites = 1;
  } else if (isReplace) {
    if (replaceIndex && !_info._input1RegisterId.has_value()) {
      // we have a FOR .. IN FILTER doc._key == ... REPLACE - no WITH.
      // in this case replace needs to behave as if it was UPDATE.
      result = _info._trx->update(_info._aqlCollection->name(), inSlice, _info._options);
    } else {
      result = _info._trx->replace(_info._aqlCollection->name(), inSlice, _info._options);
    }
    possibleWrites = 1;
  } else if (isUpdate) {
    result = _info._trx->update(_info._aqlCollection->name(), inSlice, _info._options);
    possibleWrites = 1;
  }

  // check operation result
  if (!result.ok()) {
    if (result.is(TRI_ERROR_ARANGO_DOCUMENT_NOT_FOUND) &&
        (isIndex || (isUpdate && replaceIndex) || (isUpdate && replaceIndex) ||
         (isRemove && replaceIndex) || (isReplace && replaceIndex))) {
      // document not there is not an error in this situation.
      // FOR ... FILTER ... REMOVE wouldn't invoke REMOVE in first place, so
      // don't throw an excetpion.
      return false;
    } else if (!_info._ignoreErrors) {  // TODO remove if
      THROW_ARANGO_EXCEPTION_MESSAGE(result.errorNumber(), result.errorMessage());
    }

    if (isIndex) {
      return false;
    }
  }

  stats.addWritesExecuted(possibleWrites);
  // FIXME _engine->_stats.scannedIndex++;

  if (!(_info._outputRegisterId.has_value() || _info._outputOldRegisterId.has_value() ||
        _info._outputNewRegisterId.has_value())) {
    // FIXME  return node->hasParent();
  }

  // Fill itemblock
  // create block that can hold a result with one entry and a number of
  // variables corresponding to the amount of out variables

  // only copy 1st row of registers inherited from previous frame(s)
  TRI_ASSERT(result.ok());
  VPackSlice outDocument = VPackSlice::nullSlice();
  if (result.buffer) {
    outDocument = result.slice().resolveExternal();
  }

  VPackSlice oldDocument = VPackSlice::nullSlice();
  VPackSlice newDocument = VPackSlice::nullSlice();
  if (outDocument.isObject()) {
    if (_info._outputNewRegisterId.has_value() && outDocument.hasKey("new")) {
      newDocument = outDocument.get("new");
    }
    if (outDocument.hasKey("old")) {
      outDocument = outDocument.get("old");
      if (_info._outputOldRegisterId.has_value()) {
        oldDocument = outDocument;
      }
    }
  }

  TRI_ASSERT(_info._outputRegisterId || _info._outputOldRegisterId.has_value() ||
             _info._outputNewRegisterId.has_value());

  // place documents as in the out variable slots of the result
  if (_info._outputRegisterId.has_value()) {
    AqlValue value(outDocument);
    AqlValueGuard guard(value, true);
    output.moveValueInto(_info._outputRegisterId.value(), input, guard);
  }

  if (_info._outputOldRegisterId.has_value()) {
    TRI_ASSERT(options.returnOld);
    AqlValue value(oldDocument);
    AqlValueGuard guard(value, true);
    output.moveValueInto(_info._outputOldRegisterId.value(), input, guard);
  }

  if (_info._outputNewRegisterId.has_value()) {
    TRI_ASSERT(options.returnNew);
    AqlValue value(newDocument);
    AqlValueGuard guard(value, true);
    output.moveValueInto(_info._outputNewRegisterId.value(), input, guard);
  }

  TRI_IF_FAILURE("SingleRemoteModificationOperationBlock::moreDocuments") {
    THROW_ARANGO_EXCEPTION(TRI_ERROR_DEBUG);
  }

  return true;
}

template struct ::arangodb::aql::SingleRemoteModificationExecutor<Insert>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Remove>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Replace>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Update>;
template struct ::arangodb::aql::SingleRemoteModificationExecutor<Upsert>;
