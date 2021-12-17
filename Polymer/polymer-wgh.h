/*
 * This code is part of the project "NUMA-aware Graph-structured Analytics"
 *
 *
 * Copyright (C) 2014 Institute of Parallel And Distributed Systems (IPADS), Shanghai Jiao Tong University
 *     All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 * For more about this software, visit:
 *
 *     http://ipads.se.sjtu.edu.cn/projects/polymer.html
 *
 */

#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <sys/mman.h>

#include "custom-barrier.h"
#include "parallel.h"
#include "gettime.h"
#include "utils.h"
#include "graph.h"
#include "IO.h"

#include <numa.h>
#include <pthread.h>

using namespace std;

#define PAGESIZE (4096)

#define MIN(x, y) ((x > y) ? (y) : (x))

//*****START FRAMEWORK*****

/* This is no longer ligra,
the vertices set should be set by each node
*/

inline int SXCHG(char *ptr, char newv) {
  char ret = newv;
  __asm__ __volatile__ (
                "  xchgb %0,%1\n"
                : "=r" (ret)
		: "m" (*(volatile char *)ptr), "0" (newv)
                : "memory");
  return ret;
}

long long roundUp(double x) {
    long long ones = x / 1;
    double others = x - ones;
    if (others > 0) {
	ones++;
    }
    return ones;
}

struct Subworker_Partitioner {
    int tid;
    int subTid;
    int numOfSub;
    intT dense_start;
    intT dense_end;
    pthread_barrier_t *global_barr;
    pthread_barrier_t *local_barr;
    Custom_barrier local_custom;
    Custom_barrier subMaster_custom;

    Subworker_Partitioner(int nSub):numOfSub(nSub){}

    inline bool isMaster() {return (tid + subTid == 0);}
    inline bool isSubMaster() {return (subTid == 0);}
    inline intT getStartPos(intT m) {return subTid * (m / numOfSub);}
    inline intT getEndPos(intT m) {return (subTid == numOfSub - 1) ? m : ((subTid + 1) * (m / numOfSub));}

    inline void localWait() {
	local_custom.wait();
    }
    inline void globalWait() {
	local_custom.wait();
	if (isSubMaster()) {
	    subMaster_custom.wait();
	}
	local_custom.wait();
    }
};

struct Default_Hash_F {
    int shardNum;
    intT vertPerShard;
    intT n;
    Default_Hash_F(intT _n, int _shardNum):n(_n), shardNum(_shardNum), vertPerShard(_n / _shardNum){}

    inline intT hashFunc(intT index) {
        if (index >= shardNum * vertPerShard) {
            return index;
        }
        intT idxOfShard = index % shardNum;
        intT idxInShard = index / shardNum;
        return (idxOfShard * vertPerShard + idxInShard);
    }

    inline intT hashBackFunc(intT index) {
        if (index >= shardNum * vertPerShard) {
            return index;
        }
        intT idxOfShard = index / vertPerShard;
        intT idxInShard = index % vertPerShard;
        return (idxOfShard + idxInShard * shardNum);
    }
};

template <class vertex>
void partitionByDegree(wghGraph<vertex> GA, int numOfShards, intT *sizeArr, int sizeOfOneEle, bool useOutDegree=false) {
    const intT n = GA.n;
    intT *degrees = newA(intT, n);

    intT shardSize = n / numOfShards;

    if (useOutDegree) {
	{parallel_for(intT i = 0; i < n; i++) degrees[i] = GA.V[i].getOutDegree();}
    } else {
	{parallel_for(intT i = 0; i < n; i++) degrees[i] = GA.V[i].getInDegree();}
    }

    intT accum[numOfShards];
    for (int i = 0; i < numOfShards; i++) {
	accum[i] = 0;
	sizeArr[i] = 0;
    }

    long long totalDegree = 0;
    for (intT i = 0; i < n; i++) {
	totalDegree += degrees[i];
    }

    intT averageDegree = totalDegree / numOfShards;
    intT counter = 0;
    intT tmpSizeCounter = 0;
    for (intT i = 0; i < n; i+=PAGESIZE/sizeOfOneEle) {
	for (intT j = 0; j < PAGESIZE / sizeOfOneEle; j++) {
	    if (i + j >= n)
		break;
	    accum[counter] += degrees[i + j];
	    sizeArr[counter]++;
	    tmpSizeCounter++;
	}
	if (accum[counter] >= averageDegree && counter < numOfShards - 1) {
	    counter++;
	    //cout << tmpSizeCounter / (double)(PAGESIZE / sizeOfOneEle) << endl;
	    tmpSizeCounter = 0;
	}
    }

    for (int i = 0; i < numOfShards; i++) {
	printf("%d shard: %" PRIintT "\n", i, accum[i]);
    }

    free(degrees);
}

template <class vertex>
void subPartitionByDegree(wghGraph<vertex> GA, int numOfShards, intT *sizeArr, int sizeOfOneEle, bool useOutDegree=false, bool useFakeDegree=false) {
    const intT n = GA.n;
    intT *degrees = newA(intT, n);

    intT shardSize = n / numOfShards;

    if (useFakeDegree) {
	{parallel_for(intT i = 0; i < n; i++) degrees[i] = GA.V[i].getFakeDegree();}
    } else {
	if (useOutDegree) {
	    {parallel_for(intT i = 0; i < n; i++) degrees[i] = GA.V[i].getOutDegree();}
	} else {
	    {parallel_for(intT i = 0; i < n; i++) degrees[i] = GA.V[i].getInDegree();}
	}
    }

    intT accum[numOfShards];
    for (intT i = 0; i < numOfShards; i++) {
	accum[i] = 0;
	sizeArr[i] = 0;
    }

    long long totalDegree = 0;
    for (intT i = 0; i < n; i++) {
	totalDegree += degrees[i];
    }


    intT averageDegree = totalDegree / numOfShards;
    intT counter = 0;
    intT tmpSizeCounter = 0;
    for (intT i = 0; i < n; i++) {
	accum[counter] += degrees[i];
	sizeArr[counter]++;
	tmpSizeCounter++;
	if (accum[counter] >= averageDegree && counter < numOfShards - 1) {
	    counter++;
	    //cout << tmpSizeCounter / (double)(PAGESIZE / sizeOfOneEle) << endl;
	    tmpSizeCounter = 0;
	}
    }

    free(degrees);
}

template <class vertex>
void subPartitionByDegree(wghGraph<vertex> GA, int numOfShards, intT *sizeArr, int sizeOfOneEle, intT subStart, intT subEnd, bool useOutDegree=false, bool useFakeDegree=false) {
    const intT n = subEnd - subStart;
    intT *degrees = newA(intT, n);

    intT shardSize = n / numOfShards;

    if (useFakeDegree) {
	{parallel_for(intT i = subStart; i < subEnd; i++) degrees[i-subStart] = GA.V[i].getFakeDegree();}
    } else {
	if (useOutDegree) {
	    {parallel_for(intT i = subStart; i < subEnd; i++) degrees[i-subStart] = GA.V[i].getOutDegree();}
	} else {
	    {parallel_for(intT i = subStart; i < subEnd; i++) degrees[i-subStart] = GA.V[i].getInDegree();}
	}
    }

    intT accum[numOfShards];
    for (intT i = 0; i < numOfShards; i++) {
	accum[i] = 0;
	sizeArr[i] = 0;
    }

    long long totalDegree = 0;
    for (intT i = 0; i < n; i++) {
	totalDegree += degrees[i];
    }

    intT averageDegree = totalDegree / numOfShards;
    intT counter = 0;
    intT tmpSizeCounter = 0;
    for (intT i = 0; i < n; i++) {
	accum[counter] += degrees[i];
	sizeArr[counter]++;
	tmpSizeCounter++;
	if (accum[counter] >= averageDegree && counter < numOfShards - 1) {
	    counter++;
	    //cout << tmpSizeCounter / (double)(PAGESIZE / sizeOfOneEle) << endl;
	    tmpSizeCounter = 0;
	}
    }

    free(degrees);
}

template <class vertex, class Hash_F>
void graphHasher(wghGraph<vertex> &GA, Hash_F hash) {
    vertex *V = GA.V;
    vertex *newVertexSet = (vertex *)malloc(sizeof(vertex) * GA.n);

    {parallel_for (intT i = 0; i < GA.n; i++) {
	    intT d = V[i].getOutDegree();
	    //V[i].setFakeDegree(d);
	    intE *outEdges = V[i].getOutNeighborPtr();
	    for (intT j = 0; j < d; j++) {
		outEdges[2*j] = hash.hashFunc(outEdges[2*j]);
	    }
	    newVertexSet[hash.hashFunc(i)] = V[i];
	}
    }
    GA.V = newVertexSet;
    free(V);
}

template <class vertex, class Hash_F>
void graphInEdgeHasher(wghGraph<vertex> &GA, Hash_F hash) {
    vertex *V = GA.V;
    vertex *newVertexSet = (vertex *)malloc(sizeof(vertex) * GA.n);

    {parallel_for (intT i = 0; i < GA.n; i++) {
	    intT d = V[i].getInDegree();
	    //V[i].setFakeDegree(d);
	    intE *inEdges = V[i].getInNeighborPtr();
	    for (intT j = 0; j < d; j++) {
		inEdges[2*j] = hash.hashFunc(inEdges[j]);
	    }
	    newVertexSet[hash.hashFunc(i)] = V[i];
	}
    }
    GA.V = newVertexSet;
    free(V);
}

template <class vertex, class Hash_F>
void graphAllEdgeHasher(wghGraph<vertex> &GA, Hash_F hash) {
    vertex *V = GA.V;
    vertex *newVertexSet = (vertex *)malloc(sizeof(vertex) * GA.n);

    {parallel_for (intT i = 0; i < GA.n; i++) {
	    intT d = V[i].getOutDegree();
	    intE *outEdges = V[i].getOutNeighborPtr();
	    for (intT j = 0; j < d; j++) {
		outEdges[2*j] = hash.hashFunc(outEdges[2*j]);
	    }

	    d = V[i].getInDegree();
	    intE *inEdges = V[i].getInNeighborPtr();
	    for (intT j = 0; j < d; j++) {
		inEdges[2*j] = hash.hashFunc(inEdges[j]);
	    }
	    newVertexSet[hash.hashFunc(i)] = V[i];
	}
    }
    GA.V = newVertexSet;
    free(V);
}

template <class vertex>
wghGraph<vertex> graphFilter(wghGraph<vertex> &GA, intT rangeLow, intT rangeHi, bool useOutEdge=true) {
    vertex *V = GA.V;
    vertex *newVertexSet = (vertex *)numa_alloc_local(sizeof(vertex) * GA.n);
    intT *counters = (intT *)numa_alloc_local(sizeof(intT) * GA.n);
    intT *offsets = (intT *)numa_alloc_local(sizeof(intT) * GA.n);
    {parallel_for (intT i = 0; i < GA.n; i++) {
	    intT d = (useOutEdge) ? (V[i].getOutDegree()) : (V[i].getInDegree());
	    //V[i].setFakeDegree(d);
	    newVertexSet[i].setOutDegree(V[i].getOutDegree());
	    newVertexSet[i].setInDegree(V[i].getInDegree());

	    counters[i] = 0;
	    for (intT j = 0; j < d; j++) {
		intT ngh = (useOutEdge) ? (V[i].getOutNeighbor(j)) : (V[i].getInNeighbor(j));
		if (rangeLow <= ngh && ngh < rangeHi)
		    counters[i]++;
	    }
	    newVertexSet[i].setFakeDegree(counters[i]);
	}
    }

    long long totalSize = 0;
    for (intT i = 0; i < GA.n; i++) {
	offsets[i] = totalSize;
	totalSize += counters[i];
    }
    printf("totalSize of %" PRIintT ": %lld %lld\n", rangeLow, totalSize, totalSize * 2);
    numa_free(counters, sizeof(intT) * GA.n);

    //intE *edges = (intE *)numa_alloc_local(sizeof(intE) * totalSize * 2);
    intE *edges = (intE *)malloc((long long)sizeof(intE) * totalSize * (long long)2);

    {parallel_for (intT i = 0; i < GA.n; i++) {
	    intE *localEdges = &edges[offsets[i]*2];
	    intT counter = 0;
	    intT d = (useOutEdge) ? (V[i].getOutDegree()) : (V[i].getInDegree());
	    for (intT j = 0; j < d; j++) {
		intT ngh = (useOutEdge) ? (V[i].getOutNeighbor(j)) : (V[i].getInNeighbor(j));
		intT wgh = (useOutEdge) ? (V[i].getOutWeight(j)) : (V[i].getInWeight(j));
		if (rangeLow <= ngh && ngh < rangeHi) {
		    localEdges[counter * 2] = ngh;
		    localEdges[counter * 2 + 1] = wgh;
		    counter++;
		}
	    }
	    if (counter != newVertexSet[i].getFakeDegree()) {
		printf("oops: %" PRIintT " %" PRIintT "\n", counter, newVertexSet[i].getFakeDegree());
	    }
	    if (i == 0) {
		printf("fake deg: %" PRIintT "\n", newVertexSet[i].getFakeDegree());
	    }
	    if (useOutEdge)
		newVertexSet[i].setOutNeighbors(localEdges);
	    else
		newVertexSet[i].setInNeighbors(localEdges);
	}
    }
    numa_free(offsets, sizeof(intT) * GA.n);
    //printf("degree: %" PRIintT "\n", newVertexSet[0].getFakeDegree());
    return wghGraph<vertex>(newVertexSet, GA.n, GA.m);
}

template <class vertex>
wghGraph<vertex> graphFilter2Direction(wghGraph<vertex> &GA, intT rangeLow, intT rangeHi, bool useOutEdge=true) {
    vertex *V = GA.V;
    vertex *newVertexSet = (vertex *)numa_alloc_local(sizeof(vertex) * GA.n);
    intT *counters = (intT *)numa_alloc_local(sizeof(intT) * GA.n);
    intT *offsets = (intT *)numa_alloc_local(sizeof(intT) * GA.n);
    intT *inCounters = (intT *)numa_alloc_local(sizeof(intT) * GA.n);
    intT *inOffsets = (intT *)numa_alloc_local(sizeof(intT) * GA.n);
    {parallel_for (intT i = 0; i < GA.n; i++) {
	    intT d = (useOutEdge) ? (V[i].getOutDegree()) : (V[i].getInDegree());
	    //V[i].setFakeDegree(d);
	    newVertexSet[i].setOutDegree(V[i].getOutDegree());
	    newVertexSet[i].setInDegree(V[i].getInDegree());

	    counters[i] = 0;
	    for (intT j = 0; j < d; j++) {
		intT ngh = V[i].getOutNeighbor(j);
		if (rangeLow <= ngh && ngh < rangeHi)
		    counters[i]++;
	    }

	    d = V[i].getInDegree();
	    inCounters[i] = 0;
	    for (intT j = 0; j < d; j++) {
		intT ngh = V[i].getInNeighbor(j);
		if (rangeLow <= ngh && ngh < rangeHi)
		    inCounters[i]++;
	    }
	    newVertexSet[i].setFakeDegree(counters[i]);
	    newVertexSet[i].setFakeInDegree(inCounters[i]);
	}
    }

    long long totalSize = 0;
    long long totalInSize = 0;
    for (intT i = 0; i < GA.n; i++) {
	offsets[i] = totalSize;
	totalSize += counters[i];

	inOffsets[i] = totalInSize;
	totalInSize += inCounters[i];
    }
    printf("totalSize of %" PRIintT ": %lld %lld\n", rangeLow, totalSize, totalSize * 2);
    numa_free(counters, sizeof(intT) * GA.n);
    numa_free(inCounters, sizeof(intT) * GA.n);

    //intE *edges = (intE *)numa_alloc_local(sizeof(intE) * totalSize * 2);
    intE *edges = (intE *)malloc((long long)sizeof(intE) * totalSize * (long long)2);
    intE *inEdges = (intE *)malloc((long long)sizeof(intE) * totalInSize * (long long)2);

    {parallel_for (intT i = 0; i < GA.n; i++) {
	    intE *localEdges = &edges[offsets[i]*2];
	    intT counter = 0;
	    intT d = V[i].getOutDegree();
	    for (intT j = 0; j < d; j++) {
		intT ngh = V[i].getOutNeighbor(j);
		intT wgh = V[i].getOutWeight(j);
		if (rangeLow <= ngh && ngh < rangeHi) {
		    localEdges[counter * 2] = ngh;
		    localEdges[counter * 2 + 1] = wgh;
		    counter++;
		}
	    }
	    if (counter != newVertexSet[i].getFakeDegree()) {
		printf("oops: %" PRIintT " %" PRIintT "\n", counter, newVertexSet[i].getFakeDegree());
	    }

	    intE *localInEdges = &inEdges[inOffsets[i]*2];
	    counter = 0;
	    d = V[i].getInDegree();
	    for (intT j = 0; j < d; j++) {
		intT ngh = V[i].getInNeighbor(j);
		intT wgh = V[i].getInWeight(j);
		if (rangeLow <= ngh && ngh < rangeHi) {
		    localInEdges[counter * 2] = ngh;
		    localInEdges[counter * 2 + 1] = wgh;
		    counter++;
		}
	    }

	    if (counter != newVertexSet[i].getFakeInDegree()) {
		printf("oops: %" PRIintT " %" PRIintT "\n", counter, newVertexSet[i].getFakeInDegree());
	    }

	    if (i == 0) {
		printf("fake deg: %" PRIintT "\n", newVertexSet[i].getFakeDegree());
	    }

	    newVertexSet[i].setOutNeighbors(localEdges);
	    newVertexSet[i].setInNeighbors(localInEdges);
	}
    }
    numa_free(offsets, sizeof(intT) * GA.n);
    //printf("degree: %" PRIintT "\n", newVertexSet[0].getFakeDegree());
    return wghGraph<vertex>(newVertexSet, GA.n, GA.m);
}

void *mapDataArray(int numOfShards, intT *sizeArr, int sizeOfOneEle) {
    intT numOfPages = 0;
    for (int i = 0; i < numOfShards; i++) {
        numOfPages += sizeArr[i] / (double)(PAGESIZE / sizeOfOneEle);
    }
    numOfPages++;

    void *toBeReturned = mmap(NULL, numOfPages * PAGESIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (toBeReturned == NULL) {
	cout << "OOps" << endl;
    }

    intT offset = 0;
    for (int i = 0; i < numOfShards; i++) {
	void *startPos = (void *)((char *)toBeReturned + offset * sizeOfOneEle);
	//printf("start binding %d : %" PRIintT "\n", i, offset);
	numa_tonode_memory(startPos, sizeArr[i], i);
	offset = offset + sizeArr[i];
    }
    return toBeReturned;
}

struct AsyncChunk {
    intT accessCounter;
    intT m;
    intT *s;
};

struct LocalFrontier {
    intT n;
    intT m;
    intT head;
    intT tail;
    intT emptySignal;
    intT outEdgesCount;
    intT startID;
    intT endID;
    bool *b;
    intT *s;
    intT sparseCounter;
    intT **sparseChunks;
    intT *chunkSizes;
    intT *tmp;
    bool isDense;

    LocalFrontier(bool *_b, intT start, intT end):b(_b), startID(start), endID(end), n(end - start), m(0), isDense(true), s(NULL), outEdgesCount(0), sparseChunks(NULL), chunkSizes(NULL){}

    bool inRange(intT index) { return (startID <= index && index < endID);}
    inline void setBit(intT index, bool val) { b[index-startID] = val;}
    inline bool getBit(intT index) { return b[index-startID];}

    void toSparse() {
	if (isDense) {
	    if (s != NULL)
		free(s);
	    _seq<intT> R = sequence::packIndex(b, n);
	    s = R.A;
	    m = R.n;
	    {parallel_for (intT i = 0; i < m; i++) s[i] = s[i] + startID;}
	    if (m == 0) {
		printf("%p\n", s);
	    } else {
		printf("M is %" PRIintT " and first ele is %" PRIintT "\n", m, s[0]);
	    }
	}
	isDense = false;
    }

    void toDense() {
	if (!isDense) {
	    {parallel_for(intT i=0;i<n;i++) b[i] = false;}
	    {parallel_for(intT i=0;i<m;i++) b[s[i] - startID] = true;}
	}
	//printf("hehe\n");
	isDense = true;
    }

    void setSparse(intT _m, intT *_s) {
	if (s != NULL) {
	    free(s);
	}
	m = _m;
	s = _s;
	isDense = false;
    }

    bool *swapBitVector(bool *newB) {
	bool *tmp = b;
	b = newB;
	return tmp;
    }

    void clearFrontier() {
	if (s != NULL) {
	    free(s);
	}
	s = NULL;
	m = 0;
	outEdgesCount = 0;
    }
};

//*****VERTEX OBJECT*****
struct vertices {
    intT n, m;
    int numOfNodes;
    intT numOfVertices;
    intT *numOfVertexOnNode;
    intT *offsets;
    intT *numOfNonZero;
    bool** d;
    LocalFrontier **frontiers;
    LocalFrontier **nextFrontiers;
    bool isDense;
    AsyncChunk **asyncQueue;
    intT asyncEndSignal;
    intT readerTail;
    intT insertTail;

    vertices(int _numOfNodes) {
	this->numOfNodes = _numOfNodes;
	d = (bool **)malloc(numOfNodes * sizeof(bool*));
	frontiers = (LocalFrontier **)malloc(numOfNodes * sizeof(LocalFrontier*));
	nextFrontiers = (LocalFrontier **)malloc(numOfNodes * sizeof(LocalFrontier*));
	numOfVertexOnNode = (intT *)malloc(numOfNodes * sizeof(intT));
	offsets = (intT *)malloc((numOfNodes + 1) * sizeof(intT));
	numOfNonZero = (intT *)malloc(numOfNodes * sizeof(intT));
	numOfVertices = 0;
	m = -1;
    }
    /*
    void registerArr(int nodeNum, bool *arr, intT size) {
	d[nodeNum] = arr;
	numOfVertexOnNode[nodeNum] = size;
    }
    */
    void registerFrontier(int nodeNum, LocalFrontier *frontier) {
	frontiers[nodeNum] = frontier;
	numOfVertexOnNode[nodeNum] = frontier->n;
	isDense = frontier->isDense;
    }

    void toDense() {
	if (isDense)
	    return;
	else
	    isDense = true;
	for (int i = 0; i< numOfNodes; i++) {
	    //printf("todense %d start m = %" PRIintT " n = %" PRIintT "\n", i, frontiers[i]->s[frontiers[i]->m - 1], frontiers[i]->n);
	    frontiers[i]->toDense();
	}
    }

    void toSparse() {
	if (!isDense)
	    return;
	else
	    isDense = false;
	for (int i = 0; i< numOfNodes; i++) {
	    if (frontiers[i]->isDense) {
		//printf("real convert\n");
	    }
	    frontiers[i]->toSparse();
	}
    }

    intT getEdgeStat() {
	intT sum = 0;
	for (int i = 0; i < numOfNodes; i++) {
	    sum = sum + frontiers[i]->outEdgesCount;
	}
	return sum;
    }

    void calculateOffsets() {
	for (int i = 0; i < numOfNodes; i++) {
	    numOfVertices += numOfVertexOnNode[i];
	}
	offsets[0] = 0;
	for (int i = 1; i < numOfNodes; i++) {
	    offsets[i] = numOfVertexOnNode[i-1] + offsets[i-1];
	    //printf("offset of %d: %" PRIintT "\n", i, offsets[i]);
	}
	offsets[numOfNodes] = numOfVertices;
    }

    intT getSize(int nodeNum) {
	return numOfVertexOnNode[nodeNum];
    }

    intT getSparseSize(int nodeNum) {
	return numOfNonZero[nodeNum];
    }

    void calculateNumOfNonZero(int nodeNum) {
	numOfNonZero[nodeNum] = 0;
	if (false && frontiers[nodeNum]->isDense) {
	    numOfNonZero[nodeNum] = sequence::sum(frontiers[nodeNum]->b, numOfVertexOnNode[nodeNum]);
	    frontiers[nodeNum]->m = numOfNonZero[nodeNum];
	} else {
	    numOfNonZero[nodeNum] = frontiers[nodeNum]->m;
	}
	//printf("non zero count of %d: %" PRIintT "\n", nodeNum, frontiers[nodeNum]->m);
    }

    intT numNonzeros() {
	if (m < 0) {
	    intT sum = 0;
	    for (int i = 0; i < numOfNodes; i++) {
		sum = sum + numOfNonZero[i];
	    }
	    //printf("num of non zero: %" PRIintT "\n", sum);
	    m = sum;
	}
	return m;
    }

    bool isEmpty() {
	if (m < 0) {
	    intT sum = 0;
	    for (int i = 0; i < numOfNodes; i++) {
		sum = sum + numOfNonZero[i];
	    }
	    m = sum;
	}
	return (m == 0);
    }

    int getNodeNumOfIndex(intT index) {
	int result = 0;
	while (result < numOfNodes && offsets[result] <= index) {
	    result++;
	}
	return result - 1;
    }

    int getNodeNumOfSparseIndex(intT index) {
	int result = 0;
	intT accum = 0;
	while (result < numOfNodes && accum <= index) {
	    accum += numOfNonZero[result];
	    result++;
	}
	return result - 1;
    }

    intT getOffset(int nodeNum) {
	return offsets[nodeNum];
    }

    void setBit(intT index, bool bit) {
	intT accum = 0;
	int i = 0;
        while (index >= accum + numOfVertexOnNode[i]) {
	    accum += numOfVertexOnNode[i];
	    i++;
	}
	*(frontiers[i]->b + (index - accum)) = bit;
    }

    bool getBit(intT index) {
	intT accum = 0;
	int i = 0;
        while (index >= accum + numOfVertexOnNode[i]) {
	    accum += numOfVertexOnNode[i];
	    i++;
	}
	return *(frontiers[i]->b + (index - accum));
    }

    bool *getArr(int nodeNum) {
	return frontiers[nodeNum]->b;
    }

    bool *getNextArr(int nodeNum) {
	if (nextFrontiers[nodeNum] == NULL) return NULL;
	return nextFrontiers[nodeNum]->b;
    }

    intT *getSparseArr(int nodeNum) {
	return frontiers[nodeNum]->s;
    }

    LocalFrontier *getFrontier(int nodeNum) {
	return frontiers[nodeNum];
    }

    LocalFrontier *swapFrontier(int nodeNum, LocalFrontier *newOne) {
	if (nodeNum == 0) {
	    isDense = newOne->isDense;
	}
	LocalFrontier *oldOne =  frontiers[nodeNum];
	frontiers[nodeNum] = newOne;
	m = -1;
	return oldOne;
    }

    bool eq (vertices& b) {
	return false;
    }

    void print() {
    }

    void del() {
	free(offsets);
	free(numOfVertexOnNode);
	free(d);
    }
};

struct Default_worker_arg {
    void *GA;
    int maxIter;
    int tid;
    int numOfNode;
    intT rangeLow;
    intT rangeHi;
};

struct Default_subworker_arg {
    void *GA;
    int maxIter;
    int tid;
    int subTid;
    intT startPos;
    intT endPos;
    intT rangeLow;
    intT rangeHi;
    pthread_barrier_t *global_barr;
    pthread_barrier_t *node_barr;
    pthread_barrier_t *master_barr;
    LocalFrontier *localFrontier;

    volatile int *local_custom_counter;
    volatile int *local_custom_toggle;
};

struct nonNegF{bool operator() (intT a) {return (a>=0);}};

//options to edgeMap for different versions of dense edgeMap (default is DENSE)
enum options {
  DENSE, DENSE_PARALLEL, DENSE_FORWARD
};


Subworker_Partitioner dummyPartitioner(1);

//*****EDGE FUNCTIONS*****

template <class F, class vertex>
bool* edgeMapDense(wghGraph<vertex> GA, vertices* frontier, F f, LocalFrontier *next, bool parallel = 0, Subworker_Partitioner &subworker = dummyPartitioner) {
    intT numVertices = GA.n;
    intT size = next->endID - next->startID;
    vertex *G = GA.V;

    if (subworker.isSubMaster()) {
	frontier->nextFrontiers[subworker.tid] = next;
    }

    subworker.globalWait();
    intT localOffset = next->startID;
    bool *localBitVec = frontier->getArr(subworker.tid);
    int currNodeNum = 0;
    bool *currBitVector = frontier->getNextArr(currNodeNum);
    intT nextSwitchPoint = frontier->getSize(0);
    intT currOffset = 0;
    intT counter = 0;

    intT startPos = subworker.dense_start;
    intT endPos = subworker.dense_end;

    while (startPos >= nextSwitchPoint) {
	currOffset += frontier->getSize(currNodeNum);
	nextSwitchPoint += frontier->getSize(currNodeNum + 1);
	currNodeNum++;
	currBitVector = frontier->getArr(currNodeNum);
    }

    for (intT i = startPos; i < endPos; i++){
	//next->setBit(i, false);
	if (f.cond(i)) {
	    intT d = G[i].getFakeInDegree();
	    for(intT j=0; j<d; j++){
		intT ngh = G[i].getInNeighbor(j);
		if (localBitVec[ngh - localOffset] && f.updateAtomic(ngh,i,G[i].getInWeight(j))) {
		    currBitVector[i - currOffset] = true;
		}
		if(!f.cond(i)) break;
		//__builtin_prefetch(f.nextPrefetchAddr(G[i].getInNeighbor(j+3)), 1, 3);
	    }
	}
    }
    return NULL;
}

template <class F, class vertex>
bool* edgeMapDenseForward(wghGraph<vertex> GA, vertices *frontier, F f, LocalFrontier *next, bool part = false, intT start = 0, intT end = 0) {
    intT numVertices = GA.n;
    vertex *G = GA.V;

    int currNodeNum = 0;
    bool *currBitVector = frontier->getArr(currNodeNum);
    intT nextSwitchPoint = frontier->getSize(0);
    intT currOffset = 0;
    intT counter = 0;

    intT m = 0;
    intT outEdgesCount = 0;
    bool *nextB = next->b;

    intT startPos = 0;
    intT endPos = numVertices;
    if (part) {
	startPos = start;
	endPos = end;
	currNodeNum = frontier->getNodeNumOfIndex(startPos);
	//printf("nodeNum: %d %" PRIintT "\n", currNodeNum, endPos);
	currBitVector = frontier->getArr(currNodeNum);
	nextSwitchPoint = frontier->getOffset(currNodeNum+1);
	currOffset = frontier->getOffset(currNodeNum);
    }
    for (intT i=startPos; i<endPos; i++){
	if (i == nextSwitchPoint) {
	    currOffset += frontier->getSize(currNodeNum);
	    nextSwitchPoint += frontier->getSize(currNodeNum + 1);
	    currNodeNum++;
	    currBitVector = frontier->getArr(currNodeNum);
	    //printf("OK\n");
	}
	//printf("edgemap: %p\n", currBitVector);
	m += G[i].getFakeDegree();
	if (currBitVector[i-currOffset]) {
	    intT d = G[i].getFakeDegree();
	    for(intT j=0; j<d; j++){
		uintT ngh = G[i].getOutNeighbor(j);
		if (/*next->inRange(ngh) &&*/ f.cond(ngh) && f.updateAtomic(i, ngh, G[i].getOutWeight(j))) {
		    /*
		    if (!next->getBit(ngh)) {
			m++;
			outEdgesCount += G[ngh].getOutDegree();
		    }
		    */
		    //intT idx = ngh - next->startID;
		    //m += 1 - SXCHG((char *)&(nextB[idx]), 1);
		    next->setBit(ngh, true);
		}
		//__builtin_prefetch(f.nextPrefetchAddr(G[i].getOutNeighbor(j+1)), 1, 0);
	    }
	}
	//__builtin_prefetch(f.nextPrefetchAddr(i+1), 0, 3);
	//__builtin_prefetch(G[i+3].getOutNeighborPtr(), 0, 3);
    }
    //writeAdd(&(next->m), m);
    //writeAdd(&(next->outEdgesCount), outEdgesCount);
    //printf("edgeMap: %" PRIintT " %" PRIintT "\n", m, outEdgesCount);
    return NULL;
}

template <class F, class vertex>
bool* edgeMapDenseReduce(wghGraph<vertex> GA, vertices* frontier, F f, LocalFrontier *next, bool parallel = 0, Subworker_Partitioner &subworker = dummyPartitioner) {
    intT numVertices = GA.n;
    intT size = next->endID - next->startID;
    vertex *G = GA.V;

    if (subworker.isSubMaster()) {
	frontier->nextFrontiers[subworker.tid] = next;
    }

    //subworker.globalWait();
    pthread_barrier_wait(subworker.global_barr);

    intT localOffset = next->startID;
    bool *localBitVec = frontier->getArr(subworker.tid);
    int currNodeNum = 0;
    bool *currBitVector = frontier->getNextArr(currNodeNum);
    intT nextSwitchPoint = frontier->getSize(0);
    intT currOffset = 0;
    intT counter = 0;

    intT startPos = subworker.dense_start;
    intT endPos = subworker.dense_end;

    while (startPos >= nextSwitchPoint) {
	currOffset += frontier->getSize(currNodeNum);
	nextSwitchPoint += frontier->getSize(currNodeNum + 1);
	currNodeNum++;
	currBitVector = frontier->getNextArr(currNodeNum);
    }

    for (intT i = startPos; i < endPos; i++){
	//next->setBit(i, false);
	if (i >= nextSwitchPoint) {
	    currOffset += frontier->getSize(currNodeNum);
	    nextSwitchPoint += frontier->getSize(currNodeNum + 1);
	    currNodeNum++;
	    currBitVector = frontier->getNextArr(currNodeNum);
	}
	if (true || f.cond(i)) {
	    double data[2];
	    intT d = G[i].getFakeInDegree();
	    f.initFunc((void *)data);
	    bool shouldActive = false;
	    for(intT j=0; j<d; j++){
		intT ngh = G[i].getInNeighbor(j);
		if (/*localBitVec[ngh - localOffset] && */f.reduceFunc((void *)data, ngh, G[i].getInWeight(j))) {
		    currBitVector[i - currOffset] = true;
		    //shouldActive = true;
		}
		//if(!f.cond(i)) break;
		//__builtin_prefetch(f.nextPrefetchAddr(G[i].getInNeighbor(j+3)), 1, 3);
	    }
	    if (d > 0) {
		f.combineFunc((void *)data, i);
	    }
	}
    }
    return NULL;
}

#define DYNAMIC_CHUNK_SIZE (1024)

template <class F, class vertex>
bool* edgeMapDenseForwardDynamic(wghGraph<vertex> GA, vertices *frontier, F f, LocalFrontier *next, Subworker_Partitioner &subworker=dummyPartitioner) {
    intT numVertices = GA.n;
    vertex *G = GA.V;
    if (subworker.isMaster()) {
	printf("we are here\n");
    }
    int currNodeNum = 0;
    bool *currBitVector = frontier->getArr(currNodeNum);
    intT nextSwitchPoint = frontier->getSize(0);
    intT currOffset = 0;
    intT counter = 0;

    intT m = 0;
    intT outEdgesCount = 0;
    bool *nextB = next->b;

    intT *counterPtr = &(next->sparseCounter);

    intT oldStartPos = 0;

    intT startPos = __sync_fetch_and_add(counterPtr, DYNAMIC_CHUNK_SIZE);
    intT endPos = (startPos + DYNAMIC_CHUNK_SIZE > numVertices) ? (numVertices) : (startPos + DYNAMIC_CHUNK_SIZE);
    while (startPos < numVertices) {
	while (startPos >= nextSwitchPoint) {
	    currOffset += frontier->getSize(currNodeNum);
	    nextSwitchPoint += frontier->getSize(currNodeNum + 1);
	    currNodeNum++;
	    currBitVector = frontier->getArr(currNodeNum);
	}
	for (intT i=startPos; i<endPos; i++){
	    if (i == nextSwitchPoint) {
		currOffset += frontier->getSize(currNodeNum);
		nextSwitchPoint += frontier->getSize(currNodeNum + 1);
		currNodeNum++;
		currBitVector = frontier->getArr(currNodeNum);
	    }
	    m += G[i].getFakeDegree();
	    if (currBitVector[i-currOffset]) {
		intT d = G[i].getFakeDegree();
		for(intT j=0; j<d; j++){
		    uintT ngh = G[i].getOutNeighbor(j);
		    if (f.cond(ngh) && f.updateAtomic(i, ngh, G[i].getOutWeight(j))) {
			next->setBit(ngh, true);
		    }
		}
	    }
	}
	oldStartPos = startPos;
	startPos = __sync_fetch_and_add(counterPtr, DYNAMIC_CHUNK_SIZE);
	endPos = (startPos + DYNAMIC_CHUNK_SIZE > numVertices) ? (numVertices) : (startPos + DYNAMIC_CHUNK_SIZE);
    }
    return NULL;
}

template <class F, class vertex>
bool* edgeMapDenseForwardGlobalWrite(wghGraph<vertex> GA, vertices *frontier, F f, LocalFrontier *nexts[], Subworker_Partitioner &subworker) {
    intT numVertices = GA.n;
    vertex *G = GA.V;

    bool *currBitVector = frontier->getArr(subworker.tid);
    intT currOffset = frontier->getOffset(subworker.tid);
    intT counter = 0;

    intT m = 0;
    intT outEdgesCount = 0;

    intT startPos = subworker.dense_start;
    intT endPos = subworker.dense_end;

    //printf("%d %d: start-end: %" PRIintT " %" PRIintT "\n", subworker.tid, subworker.subTid, startPos, endPos);

    int currNodeNum = frontier->getNodeNumOfIndex(startPos);
    bool *nextBitVector = nexts[currNodeNum]->b;
    intT nextSwitchPoint = frontier->getOffset(currNodeNum+1);
    intT offset = frontier->getOffset(currNodeNum);

    for (intT i=startPos; i<endPos; i++) {
	if (i == nextSwitchPoint) {
	    offset += frontier->getSize(currNodeNum);
	    nextSwitchPoint += frontier->getSize(currNodeNum + 1);
	    currNodeNum++;
	    nextBitVector = nexts[currNodeNum]->b;
	}
	m += G[i].getFakeDegree();
	if (f.cond(i)) {
	    intT d = G[i].getFakeDegree();
	    for(intT j=0; j<d; j++) {
		uintT ngh = G[i].getInNeighbor(j);
		if (currBitVector[ngh-currOffset] && f.updateAtomic(ngh, i, G[i].getInWeight(j))) {
		    nextBitVector[i-offset] = true;
		}
	    }
	}
    }
    return NULL;
}

AsyncChunk *newChunk(intT blockSize) {
    AsyncChunk *myChunk = (AsyncChunk *)malloc(sizeof(AsyncChunk));
    myChunk->s = (intT *)malloc(sizeof(intT) * blockSize);
    myChunk->m = 0;
    myChunk->accessCounter = 0;
    return myChunk;
}

template <class F, class vertex>
void edgeMapSparseAsync(wghGraph<vertex> GA, vertices *frontier, F f, LocalFrontier *next, Subworker_Partitioner &subworker = dummyPartitioner) {
    const int BLOCK_SIZE = 64;

    vertex *V = GA.V;

    int tid = subworker.tid;
    volatile intT *queueHead = &(frontier->frontiers[tid]->head);
    volatile intT *queueTail = &(frontier->readerTail);
    volatile intT *insertTail = &(frontier->insertTail);
    volatile intT *endSignal = &(frontier->asyncEndSignal);
    volatile intT *localSignal = &(frontier->frontiers[tid]->emptySignal);
    volatile intT *signals[frontier->numOfNodes];
    for (int i = 0; i < frontier->numOfNodes; i++) {
	signals[i] = &(frontier->frontiers[tid]->emptySignal);
    }
    *queueHead = 0;
    *insertTail = *queueTail;
    *localSignal = 0;
    *endSignal = 0;
    pthread_barrier_wait(subworker.local_barr);
    if (subworker.isSubMaster()) {
	printf("passed barrier\n");
    }
    intT accumSize = 0;
    AsyncChunk *myChunk = newChunk(BLOCK_SIZE);
    bool shouldFinish = false;
    while (!shouldFinish) {
	//first fetch a block
	volatile intT currHead = 0;
	volatile intT currTail = 0;
	intT endPos = 0;
	AsyncChunk *currChunk = NULL;
	/*
	if (*endSignal == 1) {
	    printf("spin: %d %d %" PRIintT " %" PRIintT " %" PRIintT "\n", subworker.tid, subworker.subTid, currHead, currTail, endPos);
	}
	*/
	do {
	    currHead = *queueHead;
	    currTail = *queueTail;
	    endPos = MIN(currHead + 1, currTail);
	} while (!__sync_bool_compare_and_swap((intT *)queueHead, currHead, endPos));

	intT reallyGotOne = endPos - currHead;
	//printf("get: %" PRIintT ", %" PRIintT "\n", currHead, endPos);
	if (reallyGotOne > 0) {
	    *localSignal = 0;

	    if (*endSignal == 1) {
		//printf("possible? %" PRIintT " %" PRIintT " %" PRIintT "\n", currHead, endPos, currTail);
	    }

	    //process chunk
	    currChunk = frontier->asyncQueue[currHead % GA.n];
	    if (currChunk == NULL) {
		printf("oops: %p %p %" PRIintT " %" PRIintT " %" PRIintT "\n", currChunk, frontier->asyncQueue[currHead % GA.n], currHead, currTail, endPos);
	    }
	    //printf("chunk pointer: %p\n", currChunk);
	    intT chunkSize = currChunk->m;
	    for (intT i = 0; i < chunkSize; i++) {
		accumSize++;
		intT idx = currChunk->s[i];
		intT d = V[idx].getOutDegree();
		for (intT j = 0; j < d; j++) {
		    intT ngh = V[idx].getOutNeighbor(j);
		    if (f.cond(ngh) && f.updateAtomic(idx, ngh, V[idx].getOutWeight(j))) {
			//add ngh into chunk
			myChunk->s[myChunk->m] = ngh;
			myChunk->m += 1;
			if (myChunk->m >= BLOCK_SIZE) {
			    //if full, send it
			    intT insertPos = __sync_fetch_and_add(insertTail, 1);
			    /*
			    if (*endSignal == 1)
				printf("before insert %" PRIintT " %" PRIintT " %" PRIintT "\n", *queueTail, insertPos, *insertTail);
			    */
			    frontier->asyncQueue[insertPos % GA.n] = myChunk;
			    //__asm__ __volatile__ ("mfence\n":::);
			    while (!__sync_bool_compare_and_swap((intT *)queueTail, insertPos, insertPos+1)) {
				if (*queueTail > insertPos) {
				    break;
				    printf("pending on insert %" PRIintT " %" PRIintT " %" PRIintT "\n", *queueTail, insertPos, *insertTail);
				}

			    }
			    //printf("insert over: %" PRIintT " %" PRIintT "\n", insertPos, *queueTail);
			    myChunk = newChunk(BLOCK_SIZE);
			}
		    }
		}
	    }
	    /*
	    intT oldCounter = __sync_fetch_and_add(&(currChunk->accessCounter), 1);
	    oldCounter++;

	    if (oldCounter >= frontier->numOfNodes) {
		frontier->asyncQueue[currHead % GA.n] = NULL;
		free(currChunk);
	    }
	    */
	} else {
	    if (myChunk->m > 0) {
		//send it
		//printf("flush chunk with size: %" PRIintT "\n", myChunk->m);
		intT insertPos = __sync_fetch_and_add(insertTail, 1);
		frontier->asyncQueue[insertPos % GA.n] = myChunk;
		//__asm__ __volatile__ ("mfence\n":::);
		while (!__sync_bool_compare_and_swap((intT *)queueTail, insertPos, insertPos+1)) {
		    if (*queueTail > insertPos) {
			break;
			printf("pending on insert %" PRIintT " %" PRIintT " %" PRIintT "\n", *queueTail, insertPos, *insertTail);
		    }
		}
		//printf("insert over: %" PRIintT " %" PRIintT "\n", insertPos, *queueTail);
		//__sync_fetch_and_add(queueTail, 1);
		myChunk = newChunk(BLOCK_SIZE);
		continue;
	    }
	    //end game part
	    //pthread_barrier_wait(subworker.local_barr);
	    //printf("%d %d here %" PRIintT " %" PRIintT "\n", subworker.tid, subworker.subTid, currHead, currTail);
	    if (*endSignal == 1) {
		//printf("%d %d and here %" PRIintT " %" PRIintT "\n", subworker.tid, subworker.subTid, currHead, currTail);
		shouldFinish = true;
	    }
	    if (subworker.isMaster()) {
		//marker algorithm
		*localSignal = 1;
		int i = 0;
		int marker = 1;
		while (*localSignal == 1) {
		    //printf("checking\n");
		    i = (i + 1) % frontier->numOfNodes;
		    if (*(signals[i]) == 1) {
			marker++;
		    } else {
			marker = 0;
		    }
		    *(signals[i]) = 1;
		    if (marker > frontier->numOfNodes) {
			*endSignal = 1;
			printf("master out\n");
			shouldFinish = true;
			break;
		    }
		}
	    }
	    //pthread_barrier_wait(subworker.local_barr);
	}
    }
    //printf("end loop of %d %d: %" PRIintT "\n", subworker.tid, subworker.subTid, accumSize);
}

template <class F, class vertex>
void edgeMapSparseV3(wghGraph<vertex> GA, vertices *frontier, F f, LocalFrontier *next, bool part = false, Subworker_Partitioner &subworker = dummyPartitioner) {
    vertex *V = GA.V;
    if (part) {
	intT currM = frontier->numNonzeros();
	intT startPos = subworker.getStartPos(currM);
	intT endPos = subworker.getEndPos(currM);

	intT *mPtr = &(next->m);
	*mPtr = 0;
	next->outEdgesCount = 0;
	intT bufferLen = frontier->getEdgeStat();
	if (subworker.isSubMaster())
	    next->s = (intT *)malloc(sizeof(intT) * bufferLen);
	intT nextEdgesCount = 0;

	//pthread_barrier_wait(subworker.local_barr);
	subworker.localWait();
	intT *nextFrontier = next->s;

	if (startPos < endPos) {
	    //printf("have ele: %" PRIintT " to %" PRIintT " %d, %p\n", startPos, endPos, subworker.tid, next);
	    int currNodeNum = frontier->getNodeNumOfSparseIndex(startPos);
	    intT offset = 0;
	    for (int i = 0; i < currNodeNum; i++) {
		offset += frontier->getSparseSize(i);
	    }
	    intT *currActiveList = frontier->getSparseArr(currNodeNum);
	    intT lengthOfCurr = frontier->getSparseSize(currNodeNum) - (startPos - offset);
	    //printf("nodeNum of %d %d: %d from %" PRIintT " to %" PRIintT "\n", subworker.tid, subworker.subTid, currNodeNum, startPos, endPos);
	    for (intT i = startPos; i < endPos; i++) {
		if (lengthOfCurr <= 0) {
		    while (currNodeNum + 1 < frontier->numOfNodes && lengthOfCurr <= 0) {
			offset += frontier->getSparseSize(currNodeNum);
			currNodeNum++;
			lengthOfCurr = frontier->getSparseSize(currNodeNum);
			//printf("lengthOfCurr: %" PRIintT "\n", lengthOfCurr);
		    }
		    if (currNodeNum >= frontier->numOfNodes || lengthOfCurr <= 0) {
			printf("oops\n");
		    }
		    currActiveList = frontier->getSparseArr(currNodeNum);
		}
		intT idx = currActiveList[i - offset];
		intT d = V[idx].getFakeDegree();
		for (intT j = 0; j < d; j++) {
		    uintT ngh = V[idx].getOutNeighbor(j);
		    //printf("from %" PRIintT " to %" PRIintT " len %" PRIintE "\n", idx, ngh, V[idx].getOutWeight(j));
		    if (f.cond(ngh) && f.updateAtomic(idx, ngh, V[idx].getOutWeight(j))) {
			intT tmp = __sync_fetch_and_add(mPtr, 1);
			if (tmp >= bufferLen)
			    printf("oops\n");
			nextFrontier[tmp] = ngh;
			nextEdgesCount += V[ngh].getOutDegree();
		    }
		}
		lengthOfCurr--;
		//printf("nextM%: " PRIintT " %" PRIintT "\n", nextM, nextEdgesCount);
	    }
	}
	__sync_fetch_and_add(&(next->outEdgesCount), nextEdgesCount);
	//pthread_barrier_wait(subworker.local_barr);
	subworker.localWait();
    }
}

static int edgesTraversed = 0;

void switchFrontier(int nodeNum, vertices *V, LocalFrontier* &next) {
    LocalFrontier *current = V->getFrontier(nodeNum);
    intT size = V->getSize(nodeNum);
    /*
    for (intT i = 0; i < size; i++) {
	(current->b)[i] = false;
    }
    */
    LocalFrontier *newF = V->swapFrontier(nodeNum, next);
    next = newF;
    next->clearFrontier();
    //V->registerArr(nodeNum, tmp, V->getSize(nodeNum));
}

void clearLocalFrontier(LocalFrontier *next, int nodeNum, int subNum, int totalSub);

// decides on sparse or dense base on number of nonzeros in the active vertices
template <class F, class vertex>
void edgeMap(wghGraph<vertex> GA, vertices *V, F f, LocalFrontier *next, intT threshold = -1,
	     char option=DENSE, bool remDups=false, bool part = false, Subworker_Partitioner &subworker = dummyPartitioner) {
    intT numVertices = GA.n;
    uintT numEdges = GA.m;
    vertex *G = GA.V;
    intT m = V->numNonzeros() + V->getEdgeStat();

    /*
    if (subworker.isMaster())
	printf("%" PRIintT "\n", m);
    */
    intT start = subworker.dense_start;
    intT end = subworker.dense_end;

    if (subworker.isMaster()) {
	//printf(((m >= threshold) ? "Dense\n" : "Sparse\n"));
    }

    if (m >= threshold) {
	//Dense part
	if (subworker.isMaster()) {
	    V->toDense();
	}

	if (subworker.isSubMaster()) {
	    next->sparseCounter = 0;
	}
	clearLocalFrontier(next, subworker.tid, subworker.subTid, subworker.numOfSub);
	//pthread_barrier_wait(subworker.global_barr);
	subworker.globalWait();

	bool* R = (option == DENSE_FORWARD) ?
	    edgeMapDenseForward(GA, V, f, next, part, start, end) :
	    //edgeMapDenseForwardDynamic(GA, V, f, next, subworker) :
	    edgeMapDense(GA, V, f, next, option, subworker);
	next->isDense = true;
    } else {
	//Sparse part
	if (subworker.isMaster()) {
	    V->toSparse();
	}

	//pthread_barrier_wait(subworker.global_barr);
	subworker.globalWait();
	edgeMapSparseV3(GA, V, f, next, part, subworker);
	next->isDense = false;
    }
}

//*****VERTEX FUNCTIONS*****
template<class vertex>
void vertexCounter(wghGraph<vertex> GA, LocalFrontier *frontier, int nodeNum, int subNum, int totalSub) {
    if (!frontier->isDense)
	return;

    intT size = frontier->endID - frontier->startID;
    intT offset = frontier->startID;
    bool *b = frontier->b;
    intT subSize = size / totalSub;
    intT startPos = subSize * subNum;
    intT endPos = subSize * (subNum + 1);
    if (subNum == totalSub - 1) {
	endPos = size;
    }

    intT m = 0;
    intT outEdges = 0;

    for (intT i = startPos; i < endPos; i++) {
	if (b[i]) {
	    outEdges += GA.V[i+offset].getOutDegree();
	    m++;
	}
    }
    writeAdd(&(frontier->m), m);
    writeAdd(&(frontier->outEdgesCount), outEdges);
}

template <class F>
void vertexMap(vertices *V, F add, int nodeNum) {
    intT size = V->getSize(nodeNum);
    intT offset = V->getOffset(nodeNum);
    bool *b = V->getArr(nodeNum);
    for (intT i = 0; i < size; i++) {
	if (b[i])
	    add(i + offset);
    }
}

template <class F>
void vertexMap(vertices *V, F add, int nodeNum, int subNum, int totalSub) {
    if (V->isDense) {
	intT size = V->getSize(nodeNum);
	intT offset = V->getOffset(nodeNum);
	bool *b = V->getArr(nodeNum);
	intT subSize = size / totalSub;
	intT startPos = subSize * subNum;
	intT endPos = subSize * (subNum + 1);
	if (subNum == totalSub - 1) {
	    endPos = size;
	}

	for (intT i = startPos; i < endPos; i++) {
	    if (b[i])
		add(i + offset);
	}
    } else {
	intT size = V->frontiers[nodeNum]->m;
	intT *s = V->frontiers[nodeNum]->s;
	intT subSize = size / totalSub;
	intT startPos = subSize * subNum;
	intT endPos = subSize * (subNum + 1);
	if (subNum == totalSub - 1) {
	    endPos = size;
	}
	for (intT i = startPos; i < endPos; i++) {
	    add(s[i]);
	}
    }
}

template <class F>
void vertexMap(LocalFrontier *V, F add, int nodeNum, int subNum, int totalSub) {
    if (V->isDense) {
	intT size = V->endID - V->startID;
	intT offset = V->startID;
	bool *b = V->b;
	intT subSize = size / totalSub;
	intT startPos = subSize * subNum;
	intT endPos = subSize * (subNum + 1);
	if (subNum == totalSub - 1) {
	    endPos = size;
	}

	for (intT i = startPos; i < endPos; i++) {
	    if (b[i])
		add(i + offset);
	}
    } else {
	intT size = V->m;
	intT *s = V->s;
	intT subSize = size / totalSub;
	intT startPos = subSize * subNum;
	intT endPos = subSize * (subNum + 1);
	if (subNum == totalSub - 1) {
	    endPos = size;
	}
	for (intT i = startPos; i < endPos; i++) {
	    add(s[i]);
	}
    }
}

void clearLocalFrontier(LocalFrontier *next, int nodeNum, int subNum, int totalSub) {
    intT size = next->endID - next->startID;
    //intT offset = V->getOffset(nodeNum);
    bool *b = next->b;
    intT subSize = size / totalSub;
    intT startPos = subSize * subNum;
    intT endPos = subSize * (subNum + 1);
    if (subNum == totalSub - 1) {
	endPos = size;
    }

    for (intT i = startPos; i < endPos; i++) {
	b[i] = false;
    }
}

template <class F>
void vertexFilter(vertices *V, F filter, int nodeNum, bool *result) {
    intT size = V->getSize(nodeNum);
    intT offset = V->getOffset(nodeNum);
    bool *b = V->getArr(nodeNum);
    for (intT i = 0; i < size; i++) {
	result[i] = false;
	if (b[i])
	    result[i] = filter(i + offset);
    }
}

template <class F>
void vertexFilter(vertices *V, F filter, int nodeNum, int subNum, int totalSub, LocalFrontier *result) {
    intT size = V->getSize(nodeNum);
    intT offset = V->getOffset(nodeNum);
    bool *b = V->getArr(nodeNum);
    intT subSize = size / totalSub;
    intT startPos = subSize * subNum;
    intT endPos = subSize * (subNum + 1);
    if (subNum == totalSub - 1) {
	endPos = size;
    }

    bool *dst = result->b;
    intT m = 0;
    /*
    if (size != result->endID - result->startID || offset != result->startID)
	printf("oops\n");
    */
    for (intT i = startPos; i < endPos; i++) {
	//result->setBit(i+offset, b[i] ? (filter(i+offset)) : (false));
	if (b[i]) {
	    dst[i] = filter(i + offset);
	    if (dst[i])
		m++;
	} else {
	    dst[i] = false;
	}
    }
    //printf("filter over\n");
    writeAdd(&(result->m), m);
}
