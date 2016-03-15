////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "Store.h"

#include <velocypack/Buffer.h>
#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

#include <iostream>

using namespace arangodb::consensus;

struct NotEmpty {
  bool operator()(const std::string& s) { return !s.empty(); }
};
struct Empty {
  bool operator()(const std::string& s) { return s.empty(); }
};

std::vector<std::string> split(const std::string& value, char separator) {
  std::vector<std::string> result;
  std::string::size_type p = (value.find(separator) == 0) ? 1:0;
  std::string::size_type q;
  while ((q = value.find(separator, p)) != std::string::npos) {
    result.emplace_back(value, p, q - p);
    p = q + 1;
  }
  result.emplace_back(value, p);
  result.erase(std::find_if(result.rbegin(), result.rend(),
                            NotEmpty()).base(), result.end());
  return result;
}

Node::Node (std::string const& name) : _parent(nullptr), _name(name) {
  _value.clear();
}
Node::Node (std::string const& name, Node const* parent) :
  _parent(parent), _name(name) {
  _value.clear();
}

Node::~Node() {}

Slice Node::slice() const {
  return (_value.size()==0) ?
    Slice("\x018",&Options::Defaults):Slice(_value.data());
}

std::string const& Node::name() const {return _name;}

Node& Node::operator= (Slice const& slice) { // Assign value (become leaf)
  _children.clear();
  _value.reset();
  _value.append(reinterpret_cast<char const*>(slice.begin()), slice.byteSize());
  return *this;
}

Node& Node::operator= (Node const& node) { // Assign node
  _name = node._name;
  _type = node._type;
  _value = node._value;
  _children = node._children;
  _ttl = node._ttl;
  return *this;
}

bool Node::operator== (arangodb::velocypack::Slice const& rhs) const {
  return rhs.equals(slice());
}

/*Node& Node::parent () {
  return *_parent;
  }

Node const& Node::parent () const {
  return *_parent;
  }*/

bool Node::remove (std::string const& path) {
  std::vector<std::string> pv = split(path, '/');
  std::string key(pv.back());
  pv.pop_back();
  try {
    Node& parent = (*this)(pv);
    
    return parent.removeChild(key);
  } catch (StoreException const& e) {
    return false;
  }
}

bool Node::removeChild (std::string const& key) {
  auto found = _children.find(key);
  if (found == _children.end())
    return false;
  else
    _children.erase(found);
  return true;
}

NodeType Node::type() const {return _children.size() ? NODE : LEAF;}

Node& Node::operator [](std::string name) {
  return *_children[name];
}

bool Node::append (std::string const name, std::shared_ptr<Node> const node) {
  if (node != nullptr) {
    _children[name] = node;
  } else {
    _children[name] = std::make_shared<Node>(name);
  }
  _children[name]->_parent = this;
  return true;
}

Node& Node::operator ()(std::vector<std::string>& pv) {
  if (pv.size()) {
    std::string const key = pv[0];
    if (_children.find(key) == _children.end()) {
      _children[key] = std::make_shared<Node>(pv[0], this);
    }
    pv.erase(pv.begin());
    return (*_children[key])(pv);
  } else {
    return *this;
  }
}

Node const& Node::operator ()(std::vector<std::string>& pv) const {
  if (pv.size()) {
    std::string const key = pv[0];
    pv.erase(pv.begin());
    if (_children.find(key) == _children.end()) {
      throw StoreException("Not found");
    }
    const Node& child = *_children.at(key);
    return child(pv);
  } else {
    return *this;
  }
}
  
Node const& Node::operator ()(std::string const& path) const {
  PathType pv = split(path,'/');
  return this->operator()(pv);
}

Node& Node::operator ()(std::string const& path) {
  PathType pv = split(path,'/');
  return this->operator()(pv);
}

Node const& Node::read (std::string const& path) const {
  PathType pv = split(path,'/');
  return this->operator()(pv);
}

Node& Node::write (std::string const& path) {
  PathType pv = split(path,'/');
  return this->operator()(pv);
}

bool Node::apply (arangodb::velocypack::Slice const& slice) {
  if (slice.type() == ValueType::Object) {
    for (auto const& i : VPackObjectIterator(slice)) {
      std::string key = i.key.toString();
      key = key.substr(1,key.length()-2);
      auto found = _children.find(key);
      if (found == _children.end()) {
        _children[key] = std::make_shared<Node>(key, this);
      }
      _children[key]->apply(i.value);
    }
  } else {
    *this = slice;
  }
  return true;
}

void Node::toBuilder (Builder& builder) const {
  try {
    if (type()==NODE) {
      VPackObjectBuilder guard(&builder);
      for (auto const& child : _children) {
        builder.add(VPackValue(child.first));
        child.second->toBuilder(builder);
      }
    } else {
      builder.add(slice());
    }
  } catch (std::exception const& e) {
    std::cout << e.what() << std::endl;
  }
}

Store::Store (std::string const& name) : Node(name) {}
Store::~Store () {}

std::vector<bool> Store::apply (query_t const& query) {    
  std::vector<bool> applied;
  std::vector<std::string> path;
  MUTEX_LOCKER(storeLocker, _storeLock);
  for (auto const& i : VPackArrayIterator(query->slice())) {
    switch (i.length()) {
    case 1:
      applied.push_back(this->apply(i[0])); break; // no precond
    case 2:
      if (check(i[1])) {
        applied.push_back(this->apply(i[0]));      // precondition
      } else {
        LOG(WARN) << "Precondition failed!";
        applied.push_back(false);
      }
      break;
    default:                                 // wrong
      LOG(FATAL) << "We can only handle log entry with or without precondition!";
      applied.push_back(false);
      break;
    }
  }
  return applied;
}

bool Store::apply (arangodb::velocypack::Slice const& slice) {
  return Node::apply(slice);
}

Node const& Store::read (std::string const& path) const {
  return Node::read(path);
}

bool Store::check (arangodb::velocypack::Slice const& slice) const {
  if (slice.type() != VPackValueType::Object) {
    LOG(WARN) << "Cannot check precondition: " << slice.toJson();
    return false;
  }
  for (auto const& precond : VPackObjectIterator(slice)) {
    std::string path = precond.key.toString();
    path = path.substr(1,path.size()-2);
    if (precond.value.type() == VPackValueType::Object) { //"old", "oldEmpty", "isArray"
      for (auto const& op : VPackObjectIterator(precond.value)) {
        std::string const& oper = op.key.copyString();
        Node const& node = (*this)(path);
        if (oper == "old") {
          return (node == op.value);
        } else if ("isArray") {
          if (op.value.type()!=VPackValueType::Bool) {
            return false;
          }
          bool isArray =
            (node.type() == LEAF &&
             node.slice().type() == VPackValueType::Array);
          return op.value.getBool() ? isArray : !isArray;
        }
      }
      
    } else {
      
    }
  }
  // 
  
  return true;
}

query_t Store::read (query_t const& queries) const { // list of list of paths
  MUTEX_LOCKER(storeLocker, _storeLock);
  query_t result = std::make_shared<arangodb::velocypack::Builder>();
  if (queries->slice().type() == VPackValueType::Array) {
    result->add(VPackValue(VPackValueType::Array)); // top node array
    for (auto const& query : VPackArrayIterator(queries->slice())) {
      read (query, *result);
    } 
    result->close();
  } else {
    LOG(FATAL) << "Read queries to stores must be arrays";
  }
  return result;
}

bool Store::read (arangodb::velocypack::Slice const& query, Builder& ret) const {

  // Collect all paths
  std::list<std::string> query_strs;
  if (query.type() == VPackValueType::Array) {
    for (auto const& sub_query : VPackArrayIterator(query))
      query_strs.push_back(sub_query.copyString());
  } else if (query.type() == VPackValueType::String) {
    query_strs.push_back(query.copyString());
  } else {
    return false;
  }
  query_strs.sort();     // sort paths

  // Remove double ranges (inclusion / identity)
  for (auto i = query_strs.begin(), j = i; i != query_strs.end(); ++i) {
    if (i!=j && i->compare(0,j->size(),*j)==0) {
      *i="";
    } else {
      j = i;
    }
  }
  auto cut = std::remove_if(query_strs.begin(), query_strs.end(), Empty());
  query_strs.erase (cut,query_strs.end());

  // Create response tree 
  Node copy("copy");
  for (auto i = query_strs.begin(); i != query_strs.end(); ++i) {
    try {
      copy(*i) = (*this)(*i);
    } catch (StoreException const&) {}
  }
  // Assemble builder from response tree
  if (query_strs.size() == 1 && copy(*query_strs.begin()).type() == LEAF) {
    ret.add(copy(*query_strs.begin()).slice());
  } else {
    copy.toBuilder(ret);
  }
  
  return true;
}



