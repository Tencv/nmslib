/**
 * Non-metric Space Library
 *
 * Authors: Bilegsaikhan Naidan (https://github.com/bileg), Leonid Boytsov (http://boytsov.info).
 * With contributions from Lawrence Cayton (http://lcayton.com/) and others.
 *
 * For the complete list of contributors and further details see:
 * https://github.com/searchivarius/NonMetricSpaceLib
 *
 * Copyright (c) 2014
 *
 * This code is released under the
 * Apache License Version 2.0 http://www.apache.org/licenses/.
 *
 */
#include <cmath>
#include <memory>
#include <iostream>
// This is only for _mm_prefetch
#include <mmintrin.h>

#include "portable_simd.h"
#include "space.h"
#include "knnquery.h"
#include "knnqueue.h"
#include "rangequery.h"
#include "ported_boost_progress.h"
#include "method/small_world_rand.h"
#include "sort_arr_bi.h"

#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <typeinfo>

#define MERGE_BUFFER_ALGO_SWITCH_THRESHOLD 100

namespace similarity {

using namespace std;

template <typename dist_t>
struct IndexThreadParamsSW {
  const Space<dist_t>&                        space_;
  SmallWorldRand<dist_t>&                     index_;
  const ObjectVector&                         data_;
  size_t                                      index_every_;
  size_t                                      out_of_;
  ProgressDisplay*                            progress_bar_;
  mutex&                                      display_mutex_;
  size_t                                      progress_update_qty_;
  
  IndexThreadParamsSW(
                     const Space<dist_t>&             space,
                     SmallWorldRand<dist_t>&          index, 
                     const ObjectVector&              data,
                     size_t                           index_every,
                     size_t                           out_of,
                     ProgressDisplay*                 progress_bar,
                     mutex&                           display_mutex,
                     size_t                           progress_update_qty
                      ) : 
                     space_(space),
                     index_(index), 
                     data_(data),
                     index_every_(index_every),
                     out_of_(out_of),
                     progress_bar_(progress_bar),
                     display_mutex_(display_mutex),
                     progress_update_qty_(progress_update_qty)
                     { }
};

template <typename dist_t>
struct IndexThreadSW {
  void operator()(IndexThreadParamsSW<dist_t>& prm) {
    ProgressDisplay*  progress_bar = prm.progress_bar_;
    mutex&            display_mutex(prm.display_mutex_); 
    /* 
     * Skip the first element, it was added already
     */
    size_t nextQty = prm.progress_update_qty_;
    for (size_t id = 1; id < prm.data_.size(); ++id) {
      if (prm.index_every_ == id % prm.out_of_) {
        MSWNode* node = new MSWNode(prm.data_[id], id);
        prm.index_.add(node, prm.data_.size());
      
        if ((id + 1 >= min(prm.data_.size(), nextQty)) && progress_bar) {
          unique_lock<mutex> lock(display_mutex);
          (*progress_bar) += (nextQty - progress_bar->count());
          nextQty += prm.progress_update_qty_;
        }
      }
    }
    if (progress_bar) {
      unique_lock<mutex> lock(display_mutex);
      (*progress_bar) += (progress_bar->expected_count() - progress_bar->count());
    }
  }
};

template <typename dist_t>
SmallWorldRand<dist_t>::SmallWorldRand(bool PrintProgress,
                                       const Space<dist_t>& space,
                                       const ObjectVector& data) : 
                                       space_(space), data_(data), PrintProgress_(PrintProgress), use_proxy_dist_(false) {}

template <typename dist_t>
void SmallWorldRand<dist_t>::CreateIndex(const AnyParams& IndexParams) 
{
  AnyParamManager pmgr(IndexParams);

  pmgr.GetParamOptional("NN",                 NN_,                  10);
  pmgr.GetParamOptional("efConstruction",     efConstruction_,      NN_);
  efSearch_ = NN_;
  pmgr.GetParamOptional("indexThreadQty",     indexThreadQty_,      thread::hardware_concurrency());
  pmgr.GetParamOptional("useProxyDist",       use_proxy_dist_,      false);

  LOG(LIB_INFO) << "NN                  = " << NN_;
  LOG(LIB_INFO) << "efConstruction_     = " << efConstruction_;
  LOG(LIB_INFO) << "indexThreadQty      = " << indexThreadQty_;
  LOG(LIB_INFO) << "useProxyDist        = " << use_proxy_dist_;

  pmgr.CheckUnused();

  SetQueryTimeParams(getEmptyParams());

  if (data_.empty()) return;

  // 2) One entry should be added before all the threads are started, or else add() will not work properly
  addCriticalSection(new MSWNode(data_[0], 0 /* id == 0 */));
  pEntryPoint_ = ElList_.begin()->second;

  unique_ptr<ProgressDisplay> progress_bar(PrintProgress_ ?
                                new ProgressDisplay(data_.size(), cerr)
                                :NULL);

  if (indexThreadQty_ <= 1) {
    // Skip the first element, one element is already added
    if (progress_bar) ++(*progress_bar);
    for (size_t id = 1; id < data_.size(); ++id) {
      MSWNode* node = new MSWNode(data_[id], id);
      add(node, data_.size());
      if (progress_bar) ++(*progress_bar);
    }
  } else {
    vector<thread>                                    threads(indexThreadQty_);
    vector<shared_ptr<IndexThreadParamsSW<dist_t>>>   threadParams; 
    mutex                                             progressBarMutex;

    for (size_t i = 0; i < indexThreadQty_; ++i) {
      threadParams.push_back(shared_ptr<IndexThreadParamsSW<dist_t>>(
                              new IndexThreadParamsSW<dist_t>(space_, *this, data_, i, indexThreadQty_,
                                                              progress_bar.get(), progressBarMutex, 200)));
    }
    for (size_t i = 0; i < indexThreadQty_; ++i) {
      threads[i] = thread(IndexThreadSW<dist_t>(), ref(*threadParams[i]));
    }
    for (size_t i = 0; i < indexThreadQty_; ++i) {
      threads[i].join();
    }
    if (ElList_.size() != data_.size()) {
      stringstream err;
      err << "Bug: ElList_.size() (" << ElList_.size() << ") isn't equal to data_.size() (" << data_.size() << ")";
      LOG(LIB_INFO) << err.str();
      throw runtime_error(err.str());
    }
    LOG(LIB_INFO) << indexThreadQty_ << " indexing threads have finished";
  }
}

template <typename dist_t>
void 
SmallWorldRand<dist_t>::SetQueryTimeParams(const AnyParams& QueryTimeParams) {
  AnyParamManager pmgr(QueryTimeParams);
  pmgr.GetParamOptional("efSearch", efSearch_, NN_);
  string tmp;
  //pmgr.GetParamOptional("algoType", tmp, "v1merge");
  pmgr.GetParamOptional("algoType", tmp, "old");
  ToLower(tmp);
  if (tmp == "v1merge") searchAlgoType_ = kV1Merge;
  else if (tmp == "old") searchAlgoType_ = kOld;
  else {
    throw runtime_error("algoType should be one of the following: old, v1merge");
  }
  pmgr.CheckUnused();
  LOG(LIB_INFO) << "Set SmallWorldRand query-time parameters:";
  LOG(LIB_INFO) << "efSearch           =" << efSearch_;
  LOG(LIB_INFO) << "algoType           =" << searchAlgoType_;
}

template <typename dist_t>
const std::string SmallWorldRand<dist_t>::StrDesc() const {
  return METH_SMALL_WORLD_RAND;
}

template <typename dist_t>
SmallWorldRand<dist_t>::~SmallWorldRand() {
}

template <typename dist_t>
void 
SmallWorldRand<dist_t>::searchForIndexing(const Object *queryObj,
                                          priority_queue<EvaluatedMSWNodeDirect<dist_t>> &resultSet,
                                          IdType maxInternalId) const
{
/*
 * The trick of using large dense bitsets instead of unordered_set was 
 * borrowed from Wei Dong's kgraph: https://github.com/aaalgo/kgraph
 *
 * This trick works really well even in a multi-threaded mode. Indeed, the amount
 * of allocated memory is small. For example, if one has 8M entries, the size of
 * the bitmap is merely 1 MB. Furthermore, setting 1MB of entries to zero via memset would take only
 * a fraction of millisecond.
 */
  vector<bool>                        visitedBitset(maxInternalId + 1); // seems to be working efficiently even in a multi-threaded mode.

  vector<MSWNode*> neighborCopy;

  /**
   * Search for the k most closest elements to the query.
   */
  MSWNode* provider = pEntryPoint_;
  CHECK_MSG(provider != nullptr, "Bug: there is not entry point set!")

  priority_queue <dist_t>                     closestDistQueue;                      
  priority_queue <EvaluatedMSWNodeReverse<dist_t>>   candidateSet; 

  dist_t d = use_proxy_dist_ ?  space_.ProxyDistance(provider->getData(), queryObj) : 
                                space_.IndexTimeDistance(provider->getData(), queryObj);
  EvaluatedMSWNodeReverse<dist_t> ev(d, provider);

  candidateSet.push(ev);
  closestDistQueue.push(d);

  if (closestDistQueue.size() > efConstruction_) {
    closestDistQueue.pop();
  }

  size_t nodeId = provider->getId();
  if (nodeId > maxInternalId) {
    stringstream err;
    err << "Bug: nodeId > maxInternalId";
    LOG(LIB_INFO) << err.str();
    throw runtime_error(err.str());
  }
  visitedBitset[nodeId] = true;
  resultSet.emplace(d, provider);
      
  if (resultSet.size() > NN_) { // TODO check somewhere that NN > 0
    resultSet.pop();
  }        

  while (!candidateSet.empty()) {
    const EvaluatedMSWNodeReverse<dist_t>& currEv = candidateSet.top();
    dist_t lowerBound = closestDistQueue.top();

    /*
     * Check if we reached a local minimum.
     */
    if (currEv.getDistance() > lowerBound) {
      break;
    }
    MSWNode* currNode = currEv.getMSWNode();

    /*
     * This lock protects currNode from being modified
     * while we are accessing elements of currNode.
     */
    size_t neighborQty = 0;
    {
      unique_lock<mutex> lock(currNode->accessGuard_);

      //const vector<MSWNode*>& neighbor = currNode->getAllFriends();
      const vector<MSWNode*>& neighbor = currNode->getAllFriends();
      neighborQty = neighbor.size();
      if (neighborQty > neighborCopy.size()) neighborCopy.resize(neighborQty);
      for (size_t k = 0; k < neighborQty; ++k)
        neighborCopy[k]=neighbor[k];
    }

    // Can't access curEv anymore! The reference would become invalid
    candidateSet.pop();

    // calculate distance to each neighbor
    for (size_t neighborId = 0; neighborId < neighborQty; ++neighborId) {
      MSWNode* pNeighbor = neighborCopy[neighborId];

      size_t nodeId = pNeighbor->getId();
      if (nodeId > maxInternalId) {
        stringstream err;
        err << "Bug: nodeId >  maxInternalId";
        LOG(LIB_INFO) << err.str();
        throw runtime_error(err.str());
      }
      if (!visitedBitset[nodeId]) {
        visitedBitset[nodeId] = true;
        d = use_proxy_dist_ ? space_.ProxyDistance(pNeighbor->getData(), queryObj) : 
                              space_.IndexTimeDistance(pNeighbor->getData(), queryObj);

        if (closestDistQueue.size() < efConstruction_ || d < closestDistQueue.top()) {
          closestDistQueue.push(d);
          if (closestDistQueue.size() > efConstruction_) {
            closestDistQueue.pop();
          }
          candidateSet.emplace(d, pNeighbor);
        }

        if (resultSet.size() < NN_ || resultSet.top().getDistance() > d) {
          resultSet.emplace(d, pNeighbor);
          if (resultSet.size() > NN_) { // TODO check somewhere that NN > 0
            resultSet.pop();
          }
        }
      }
    }
  }
}


template <typename dist_t>
void SmallWorldRand<dist_t>::add(MSWNode *newElement, IdType maxInternalId){
  newElement->removeAllFriends(); 

  bool isEmpty = false;

  {
    unique_lock<mutex> lock(ElListGuard_);
    isEmpty = ElList_.empty();
  }

  if(isEmpty){
  // Before add() is called, the first node should be created!
    LOG(LIB_INFO) << "Bug: the list of nodes shouldn't be empty!";
    throw runtime_error("Bug: the list of nodes shouldn't be empty!");
  }

  {
    priority_queue<EvaluatedMSWNodeDirect<dist_t>> resultSet;

    searchForIndexing(newElement->getData(), resultSet, maxInternalId);

    // TODO actually we might need to add elements in the reverse order in the future.
    // For the current implementation, however, the order doesn't seem to matter
    while (!resultSet.empty()) {
      link(resultSet.top().getMSWNode(), newElement);
      resultSet.pop();
    }
  }

  addCriticalSection(newElement);
}

template <typename dist_t>
void SmallWorldRand<dist_t>::addCriticalSection(MSWNode *newElement){
  unique_lock<mutex> lock(ElListGuard_);

  ElList_.insert(make_pair(newElement->getData()->id(), newElement));
}

template <typename dist_t>
void SmallWorldRand<dist_t>::Search(RangeQuery<dist_t>* query, IdType) const {
  throw runtime_error("Range search is not supported!");
}

template <typename dist_t>
void SmallWorldRand<dist_t>::Search(KNNQuery<dist_t>* query, IdType) const {
  if (searchAlgoType_ == kV1Merge) SearchV1Merge(query);
  else SearchOld(query);
}

template <typename dist_t>
void SmallWorldRand<dist_t>::SearchV1Merge(KNNQuery<dist_t>* query) const {
  if (ElList_.empty()) return;
  CHECK_MSG(efSearch_ > 0, "efSearch should be > 0");
/*
 * The trick of using large dense bitsets instead of unordered_set was
 * borrowed from Wei Dong's kgraph: https://github.com/aaalgo/kgraph
 *
 * This trick works really well even in a multi-threaded mode. Indeed, the amount
 * of allocated memory is small. For example, if one has 8M entries, the size of
 * the bitmap is merely 1 MB. Furthermore, setting 1MB of entries to zero via memset would take only
 * a fraction of millisecond.
 */
  vector<bool>                        visitedBitset(ElList_.size());

  /**
   * Search of most k-closest elements to the query.
   */
  MSWNode* currNode = pEntryPoint_;
  CHECK_MSG(currNode != nullptr, "Bug: there is not entry point set!")

  SortArrBI<dist_t,MSWNode*> sortedArr(max<size_t>(efSearch_, query->GetK()));

  const Object* currObj = currNode->getData();
  dist_t d = query->DistanceObjLeft(currObj);
  sortedArr.push_unsorted_grow(d, currNode); // It won't grow

  size_t nodeId = currNode->getId();
  CHECK(nodeId < ElList_.size());

  visitedBitset[nodeId] = true;

  int_fast32_t  currElem = 0;

  typedef typename SortArrBI<dist_t,MSWNode*>::Item  QueueItem;

  vector<QueueItem>& queueData = sortedArr.get_data();
  vector<QueueItem>  itemBuff(8*NN_);

  // efSearch_ is always <= # of elements in the queueData.size() (the size of the BUFFER), but it can be
  // larger than sortedArr.size(), which returns the number of actual elements in the buffer
  while(currElem < min(sortedArr.size(),efSearch_)){
    auto& e = queueData[currElem];
    CHECK(!e.used);
    e.used = true;
    currNode = e.data;
    ++currElem;

    for (MSWNode* neighbor : currNode->getAllFriends()) {
      _mm_prefetch(reinterpret_cast<const char*>(const_cast<const Object*>(neighbor->getData())), _MM_HINT_T0);
    }
    for (MSWNode* neighbor : currNode->getAllFriends()) {
      _mm_prefetch(const_cast<const char*>(neighbor->getData()->data()), _MM_HINT_T0);
    }

    if (currNode->getAllFriends().size() > itemBuff.size())
      itemBuff.resize(currNode->getAllFriends().size());

    size_t itemQty = 0;

    dist_t topKey = sortedArr.top_key();
    //calculate distance to each neighbor
    for (MSWNode* neighbor : currNode->getAllFriends()) {
      nodeId = neighbor->getId();
      CHECK(nodeId < ElList_.size());

      if (!visitedBitset[nodeId]) {
        currObj = neighbor->getData();
        d = query->DistanceObjLeft(currObj);
        visitedBitset[nodeId] = true;
        if (sortedArr.size() < efSearch_ || d < topKey) {
          itemBuff[itemQty++]=QueueItem(d, neighbor);
        }
      }
    }

    if (itemQty) {
      _mm_prefetch(const_cast<const char*>(reinterpret_cast<char*>(&itemBuff[0])), _MM_HINT_T0);
      std::sort(itemBuff.begin(), itemBuff.begin() + itemQty);

      size_t insIndex=0;
      if (itemQty > MERGE_BUFFER_ALGO_SWITCH_THRESHOLD) {
        insIndex = sortedArr.merge_with_sorted_items(&itemBuff[0], itemQty);

        if (insIndex < currElem) {
          currElem = insIndex;
        }
      } else {
        for (size_t ii = 0; ii < itemQty; ++ii) {
          size_t insIndex = sortedArr.push_or_replace_non_empty_exp(itemBuff[ii].key, itemBuff[ii].data);

          if (insIndex < currElem) {
            currElem = insIndex;
          }
        }
      }
    }

    // To ensure that we either reach the end of the unexplored queue or currElem points to the first unused element
    while (currElem < sortedArr.size() && queueData[currElem].used == true)
      ++currElem;
  }

  for (int_fast32_t i = 0; i < query->GetK() && i < sortedArr.size(); ++i) {
    query->CheckAndAddToResult(queueData[i].key, queueData[i].data->getData());
  }
}


template <typename dist_t>
void SmallWorldRand<dist_t>::SearchOld(KNNQuery<dist_t>* query) const {

  if (ElList_.empty()) return;
  CHECK_MSG(efSearch_ > 0, "efSearch should be > 0");
/*
 * The trick of using large dense bitsets instead of unordered_set was
 * borrowed from Wei Dong's kgraph: https://github.com/aaalgo/kgraph
 *
 * This trick works really well even in a multi-threaded mode. Indeed, the amount
 * of allocated memory is small. For example, if one has 8M entries, the size of
 * the bitmap is merely 1 MB. Furthermore, setting 1MB of entries to zero via memset would take only
 * a fraction of millisecond.
 */
  vector<bool>                        visitedBitset(ElList_.size());

  MSWNode* provider = pEntryPoint_;
  CHECK_MSG(provider != nullptr, "Bug: there is not entry point set!")

  priority_queue <dist_t>                          closestDistQueue; //The set of all elements which distance was calculated
  priority_queue <EvaluatedMSWNodeReverse<dist_t>> candidateQueue; //the set of elements which we can use to evaluate

  const Object* currObj = provider->getData();
  dist_t d = query->DistanceObjLeft(currObj);
  query->CheckAndAddToResult(d, currObj); // This should be done before the object goes to the queue: otherwise it will not be compared to the query at all!

  EvaluatedMSWNodeReverse<dist_t> ev(d, provider);
  candidateQueue.push(ev);
  closestDistQueue.emplace(d);

  size_t nodeId = provider->getId();
  if (nodeId >= ElList_.size()) {
    stringstream err;
    err << "Bug: nodeId > ElList_.size()";
    LOG(LIB_INFO) << err.str();
    throw runtime_error(err.str());
  }
  visitedBitset[nodeId] = true;

  while(!candidateQueue.empty()){

    auto iter = candidateQueue.top(); // This one was already compared to the query
    const EvaluatedMSWNodeReverse<dist_t>& currEv = iter;

    // Did we reach a local minimum?
    if (currEv.getDistance() > closestDistQueue.top()) {
      break;
    }

    for (MSWNode* neighbor : (currEv.getMSWNode())->getAllFriends()) {
      _mm_prefetch(reinterpret_cast<const char*>(const_cast<const Object*>(neighbor->getData())), _MM_HINT_T0);
    }
    for (MSWNode* neighbor : (currEv.getMSWNode())->getAllFriends()) {
      _mm_prefetch(const_cast<const char*>(neighbor->getData()->data()), _MM_HINT_T0);
    }

    const vector<MSWNode*>& neighbor = (currEv.getMSWNode())->getAllFriends();

    // Can't access curEv anymore! The reference would become invalid
    candidateQueue.pop();

    //calculate distance to each neighbor
    for (auto iter = neighbor.begin(); iter != neighbor.end(); ++iter){
      nodeId = (*iter)->getId();
      if (nodeId >= ElList_.size()) {
        stringstream err;
        err << "Bug: nodeId > ElList_.size()";
        LOG(LIB_INFO) << err.str();
        throw runtime_error(err.str());
      }
      if (!visitedBitset[nodeId]) {
        currObj = (*iter)->getData();
        d = query->DistanceObjLeft(currObj);
        visitedBitset[nodeId] = true;

        if (closestDistQueue.size() < efSearch_ || d < closestDistQueue.top()) {
          closestDistQueue.emplace(d);
          if (closestDistQueue.size() > efSearch_) {
            closestDistQueue.pop();
          }

          candidateQueue.emplace(d, *iter);
        }

        query->CheckAndAddToResult(d, currObj);
      }
    }
  }
}

template <typename dist_t>
void SmallWorldRand<dist_t>::SaveIndex(const string &location) {
  ofstream outFile(location);
  CHECK_MSG(outFile, "Cannot open file '" + location + "' for writing");
  outFile.exceptions(std::ios::badbit);
  size_t lineNum = 0;

  WriteField(outFile, METHOD_DESC, StrDesc()); lineNum++;
  WriteField(outFile, "NN", NN_); lineNum++;

  for(ElementMap::iterator it = ElList_.begin(); it != ElList_.end(); ++it) {
    MSWNode* pNode = it->second;
    IdType nodeID = pNode->getId();
    CHECK_MSG(nodeID >= 0 && nodeID < data_.size(),
              "Bug: unexpected node ID " + ConvertToString(nodeID) +
              " for object ID " + ConvertToString(pNode->getData()->id()) +
              "data_.size() = " + ConvertToString(data_.size()));
    outFile << nodeID << ":" << pNode->getData()->id() << ":";
    for (const MSWNode* pNodeFriend: pNode->getAllFriends()) {
      IdType nodeFriendID = pNodeFriend->getId();
      CHECK_MSG(nodeFriendID >= 0 && nodeFriendID < data_.size(),
                "Bug: unexpected node ID " + ConvertToString(nodeFriendID) +
                " for object ID " + ConvertToString(pNodeFriend->getData()->id()) +
                "data_.size() = " + ConvertToString(data_.size()));
      outFile << ' ' << nodeFriendID;
    }
    outFile << endl; lineNum++;
  }
  outFile << endl; lineNum++; // The empty line indicates the end of data entries
  WriteField(outFile, LINE_QTY, lineNum + 1 /* including this line */);
  outFile.close();
}

template <typename dist_t>
void SmallWorldRand<dist_t>::LoadIndex(const string &location) {
  vector<MSWNode *> ptrMapper(data_.size());

  for (unsigned pass = 0; pass < 2; ++ pass) {
    ifstream inFile(location);
    CHECK_MSG(inFile, "Cannot open file '" + location + "' for reading");
    inFile.exceptions(std::ios::badbit);

    size_t lineNum = 1;
    string methDesc;
    ReadField(inFile, METHOD_DESC, methDesc);
    lineNum++;
    CHECK_MSG(methDesc == StrDesc(),
              "Looks like you try to use an index created by a different method: " + methDesc);
    ReadField(inFile, "NN", NN_);
    lineNum++;

    string line;
    while (getline(inFile, line)) {
      if (line.empty()) {
        lineNum++; break;
      }
      stringstream str(line);
      str.exceptions(std::ios::badbit);
      char c1, c2;
      IdType nodeID, objID;
      CHECK_MSG((str >> nodeID) && (str >> c1) && (str >> objID) && (str >> c2),
                "Bug or inconsitent data, wrong format, line: " + ConvertToString(lineNum)
      );
      CHECK_MSG(c1 == ':' && c2 == ':',
                string("Bug or inconsitent data, wrong format, c1=") + c1 + ",c2=" + c2 +
                " line: " + ConvertToString(lineNum)
      );
      CHECK_MSG(nodeID >= 0 && nodeID < data_.size(),
                DATA_MUTATION_ERROR_MSG + " (unexpected node ID " + ConvertToString(nodeID) +
                " for object ID " + ConvertToString(objID) +
                " data_.size() = " + ConvertToString(data_.size()) + ")");
      CHECK_MSG(data_[nodeID]->id() == objID,
                DATA_MUTATION_ERROR_MSG + " (unexpected object ID " + ConvertToString(data_[nodeID]->id()) +
                " for data element with ID " + ConvertToString(nodeID) +
                " expected object ID: " + ConvertToString(objID) + ")"
      );
      if (pass == 0) {
        unique_ptr<MSWNode> node(new MSWNode(data_[nodeID], nodeID));
        ptrMapper[nodeID] = node.get();
        ElList_.insert(make_pair(node->getData()->id(), node.release()));
      } else {
        MSWNode *pNode = ptrMapper[nodeID];
        CHECK_MSG(pNode != NULL,
                  "Bug, got NULL pointer in the second pass for nodeID " + ConvertToString(nodeID));
        IdType nodeFriendID;
        while (str >> nodeFriendID) {
          CHECK_MSG(nodeFriendID >= 0 && nodeFriendID < data_.size(),
                    "Bug: unexpected node ID " + ConvertToString(nodeFriendID) +
                    "data_.size() = " + ConvertToString(data_.size()));
          MSWNode *pFriendNode = ptrMapper[nodeFriendID];
          CHECK_MSG(pFriendNode != NULL,
                    "Bug, got NULL pointer in the second pass for nodeID " + ConvertToString(nodeFriendID));
          pNode->addFriend(pFriendNode, false /* don't check for duplicates */);
        }
        CHECK_MSG(str.eof(),
                  "It looks like there is some extract erroneous stuff in the end of the line " +
                  ConvertToString(lineNum));
      }
      ++lineNum;
    }

    size_t ExpLineNum;
    ReadField(inFile, LINE_QTY, ExpLineNum);
    CHECK_MSG(lineNum == ExpLineNum,
              DATA_MUTATION_ERROR_MSG + " (expected number of lines " + ConvertToString(ExpLineNum) +
              " read so far doesn't match the number of read lines: " + ConvertToString(lineNum) + ")");
    inFile.close();
  }
}

template class SmallWorldRand<float>;
template class SmallWorldRand<double>;
template class SmallWorldRand<int>;

}
