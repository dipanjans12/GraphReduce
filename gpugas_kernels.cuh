/******************************************************************************
Copyright 2013 Royal Caliber LLC. (http://www.royal-caliber.com)

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
******************************************************************************/

#ifndef GPUGAS_KERNELS_CUH__
#define GPUGAS_KERNELS_CUH__

//from moderngpu library
#include "device/ctaloadbalance.cuh"

#include <stdio.h>

//Device code for GASEngineGPU


//Ideally would have wanted these kernels to be static private member functions
//but nvcc (Cuda 5.0) does not like __global__ or __device__ code inside classes.
namespace GPUGASKernels
{


//assumes blocks are 1-D
//allows for 2D grids since 1D grids don't get us very far.
template<typename Int>
__device__ Int globalThreadId()
{
  return threadIdx.x + blockDim.x * (blockIdx.x + blockIdx.y * gridDim.x + blockIdx.z * gridDim.x * gridDim.y);
}


//set an array to a range of values
template<typename Int>
__global__ void kRange(Int start, Int end, Int *out, Int *s2vMap, Int *v2sMap)
{
  Int tid = globalThreadId<Int>();
  if( tid < end - start )
  {
    out[tid] = tid + start - s2vMap[v2sMap[tid + start]];
  }
}


//correct the indexes of active array for multiple shards
template<typename Int>
__global__ void fixRangeIn(Int *in, Int *out, Int num, Int *s2vMap, Int *v2sMap, Int *active2sMap, Int numShards)
{
  Int tid = globalThreadId<Int>();
  if( tid < num )
  {
    Int shardId = v2sMap[out[tid]];
    Int shard_begin = s2vMap[shardId];
    Int activeScan = 0;
    for(int i = 0; i < shardId; ++i)
      activeScan += active2sMap[i];

    Int new_loc = shard_begin ? (tid - activeScan) + shard_begin : tid;
    in[new_loc] = out[tid] - shard_begin;
  }
}

template<typename Int>
__global__ void fixRangeIn2(Int *out, Int num, Int *s2vMap, Int *v2sMap, Int numShards)
{
  Int tid = globalThreadId<Int>();
  if( tid < num )
  {
    Int shardId = v2sMap[out[tid]];
    out[tid] = out[tid] - s2vMap[shardId];
  }
}

//correct the indexes of active array for multiple shards
template<typename Int>
__global__ void fixRangeOut(Int *out, Int num, Int *s2vMap, Int *v2sMap, Int numShards)
{
  Int tid = globalThreadId<Int>();
  if( tid < num )
  {
    //find shard from tid and s2vMap
    //can use binary search but since numShards is generally small, we can do iteratively
    Int shardId;
    for(size_t i = 0; i < numShards; ++i)
    {
      if(s2vMap[i+1] > tid)
      {
        shardId = i;
        break;
      }
    }
    out[tid] = out[tid] + s2vMap[shardId];
  }
}


//This version does the gatherMap only and generates keys for subsequent
//use iwth thrust reduce_by_key
template<typename Program, typename Int, int NT, int indirectedGather>
__global__ void kGatherMap(Int nActiveVertices
  , const Int *activeVertices
  , const int numBlocks
  , Int nTotalEdges
  , const Int *edgeCountScan
  , const Int *mergePathPartitions
  , const Int *srcOffsets
  , const Int *srcs
  , const typename Program::VertexData* vertexData
  , const typename Program::EdgeData*   edgeData
  , const Int* edgeIndexCSC
  , const Int dstOffset
  , Int *dsts
  , typename Program::GatherResult* output)
{
  //boilerplate from MGPU, VT will be 1 in this kernel until
  //a full rewrite of this kernel, so not bothering with LaunchBox
  const int VT = 1;
  
  union Shared
  {
    Int indices[NT * (VT + 1)];
    Int dstVerts[NT * VT];
  };
  __shared__ Shared shared; //so poetic!

  Int block = blockIdx.x + blockIdx.y * gridDim.x;
  Int bTid  = threadIdx.x; //tid within block

  if (block >= numBlocks)
    return;

  //printf("bTid = %d edgeCountScan = %d\n",bTid,edgeCountScan[bTid]);

  int4 range = mgpu::CTALoadBalance<NT, VT>(nTotalEdges, edgeCountScan
    , nActiveVertices, block, bTid, mergePathPartitions
    , shared.indices, true);

  //global index into output
  Int gTid = bTid + range.x;

  //get the count of edges this block will do
  int edgeCount = range.y - range.x;

  //get the number of dst vertices this block will do
  //int nDsts = range.w - range.z;

  int iActive[VT];
  mgpu::DeviceSharedToReg<NT, VT>(shared.indices, bTid, iActive);

  //each thread that is responsible for an edge should now apply Program::gatherMap
  if( bTid < edgeCount )
  {
    //get the incoming edge index for this dstVertex
    int iEdge;

    iEdge = gTid - shared.indices[edgeCount + iActive[0] - range.z];
    typename Program::GatherResult result;
    Int dstVerts[VT];
    
    //should we use an mgpu function for this indirected load?
    Int dst = dstVerts[0] = activeVertices[iActive[0]];
    //check if we have a vertex with no incoming edges
    //this is the matching kludge for faking the count to be 1
    Int soff = srcOffsets[dst];
    Int nEdges = srcOffsets[dst + 1] - soff;
    if( nEdges )
    {
      iEdge  += soff;
      Int src = srcs[ iEdge ];
//      typename Program::VertexData v=*(vertexData+src);
//      printf("gTid=%d src=%d dst=%d rank=%f numOutedges=%d\n",gTid,src,dst+dstOffset,v.rank,v.numOutEdges);
      if( indirectedGather )
        //TODO: We have not corrected edgeIndexCSC/CSR indexes yet because it requires us to have global edges in gpu memory which is against our premise
        result = Program::gatherMap(vertexData + dst + dstOffset, vertexData + src, edgeData + edgeIndexCSC[iEdge] );
      else
        result = Program::gatherMap(vertexData + dst + dstOffset, vertexData + src, edgeData + iEdge );
    }
    else
      result = Program::gatherZero;

    //write out a key and a result.
    //Next we will be adding a blockwide or atleast a warpwide reduction here.
    //printf("iActive[0]=%d bTid=%d gtId=%d dsts=%d map=%f\n",iActive[0], bTid, gTid,dstVerts[0],result);
    dsts[gTid] = dstVerts[0];
    output[gTid] = result;
  }
}


//Run Program::apply and store which vertices are to activate their neighbors
//we can either return a compact list or a set of flags.  To be decided
template<typename Program, typename Int>
__global__ void kApply(Int nActiveVertices
  , const Int *activeVertices
  , const typename Program::GatherResult *gatherResults
  , typename Program::VertexData *vertexData
  , Int *retFlags
  , Int nVertices
  , const Int *s2vMap
  , const Int *v2sMap
  , const Int * active2sMap
  , Int numShards)
{
  Int tid = globalThreadId<Int>();
  if( tid >= nVertices )
    return;

  //find shard from tid and s2vMap
  //can use binary search but since numShards is generally small, we can do iteratively
  Int shardId;
  for(size_t i = 0; i < numShards; ++i)
  {
    if(s2vMap[i+1] > tid)
    {
      shardId = i;
      break;
    }
  }

  Int numActiveShard = active2sMap[shardId];
  Int max_tid = s2vMap[shardId] + numActiveShard;
  if(tid >= max_tid)
    return;

  int vid = activeVertices[tid];
  retFlags[tid] = Program::apply(vertexData ? vertexData + vid : 0, gatherResults[tid]);
}



} //end namespace GPUGASKernels

#if 0
template<typename Int>
__global__
void printEdgeScan(Int* edgeCountScan, Int n)
{
  int tid=threadIdx.x +blockIdx.x*blockDim.x;
  if(tid < n)
    printf("tid=%d edgeScan=%d\n",tid,edgeCountScan[tid]);
}
#endif


#endif
