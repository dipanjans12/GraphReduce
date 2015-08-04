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

#define print(str) (std::cout << (str) << std::endl)
#include <stdio.h>
#define DEBUG 0
#define SYNCD 0
#define VERBOSE 0

int flag = 0;

#ifndef GPUGAS_H__
#define GPUGAS_H__

/*
Second iteration of a CUDA implementation for GPUs.
The primary difference in this version as opposed to the first round is that we
maintain a compact list of active vertices as opposed to always working on the
entire graph.

There are pros and cons to using an active vertex list vs always working on
everything:

pros:
-  improved performance where the active set is much smaller than the whole graph

cons:
-  an active vertex list requires additional calculations to load balance
   properly.  For both gather and scatter, we need to dynamically figure out the
   mapping between threads and the edge(s) they are responsible for.

-  scattering with an active vertex list requires us to be able to look up the
   outgoing edges given a vertex id.  This means that in addition to the CSC
   representation used in the gather portion, we also need the CSR representation
   for the scatter.  This doubles the edge storage requirement.


Implementation Notes(VV):
-  decided to move away from thrust because thrust requires us to compose at the
   host level and there are unavoidable overheads to that approach.

-  between CUB and MGPU, MGPU offers a few key pieces that we want to use, namely
   LBS and IntervalMove.  Both CUB and MGPU have their own slightly different
   ways of doing things host-side.  Since neighter CUB nor MGPU seem to be stable
   APIs, this implementation chooses an MGPU/CUB-neutral way of doing things
   wherever possible.

-  Program::apply() now returns a boolean, indicating whether to active its entire
   neighborhood or not.
*/

#include "gpugas_kernels.cuh"
#include <vector>
#include <iterator>
#include <algorithm>
#include "moderngpu.cuh"
#include "primitives/scatter_if_mgpu.h"
#include "util.cuh"

//using this because CUB device-wide reduce_by_key does not yet work
//and I am still working on a fused gatherMap/gatherReduce kernel.
#include "thrust/reduce.h"
#include "thrust/device_ptr.h"


//CUDA implementation of GAS API, version 2.
template<typename Program
  , typename Int = int32_t
  , bool sortEdgesForGather = true>
class GASEngineGPUShard
{
  //public to make nvcc happy
public:
  typedef typename Program::VertexData   VertexData;
  typedef typename Program::EdgeData     EdgeData;
  typedef typename Program::GatherResult GatherResult;

private:

  Int         m_nVertices;
//  Int         m_nEdges;
  Int         m_nCSREdges;
  Int         m_nCSCEdges;

  //input/output pointers to host data
  //VertexData *m_vertexDataHost;
  //EdgeData   *m_edgeDataHost;
  bool m_vertexDataHost;
  bool m_edgeDataHost;

  //GPU copy
  VertexData *m_vertexData;
  EdgeData   *m_edgeData;
  Int        m_vertexOffset;

  //CSC representation for gather phase
  //Kernel accessible data
  Int *m_srcs; //Not required since I will be creating the CSC representation in the parent class
  Int *m_srcOffsets;
  Int *m_edgeIndexCSC;

  //CSR representation for reduce phase
  Int *m_dsts; //Not required since I will be creating the CSR representation in the parent class
  Int *m_dstOffsets;
  Int *m_edgeIndexCSR;

  //Active vertex lists
  Int *m_active;
  Int  m_nActive;
  Int *m_nActiveShard;
  Int *m_activeNext;
  Int  m_nActiveNext;
  Int *m_applyRet; //set of vertices whose neighborhood will be active next
  char *m_activeFlags;

  //some temporaries that are needed for LBS
  Int *m_edgeCountScan;

  //mapped memory to avoid explicit copy of reduced value back to host memory
  Int *m_hostMappedValue;
  Int *m_deviceMappedValue;

  //counter and list for small sized scatter / activation
  //Int *m_edgeOutputCounter;
  //Int *m_outputEdgeList;

  //These go away once gatherMap/gatherReduce/apply are fused
  GatherResult *m_gatherMapTmp;  //store results of gatherMap()
  GatherResult *m_gatherTmp;     //store results of gatherReduce()
  Int          *m_gatherDstsTmp; //keys for reduce_by_key in gatherReduce
//Don't need hosttmp because it is a O(V) array and we keep it in the GPU memory only i.e. its a global tmp
//  GatherResult *m_gatherTmpHost;     //copy back results of gatherReduce() to host memory

  //Preprocessed data for speeding up reduce_by_key when all vertices are active
  std::auto_ptr<mgpu::ReduceByKeyPreprocessData> preprocessData;
  bool preComputed;
  bool *preComputedShard;

  //MGPU context
  mgpu::ContextPtr m_mgpuContext;

  //profiling
  cudaEvent_t m_ev0, m_ev1;
  //convenience
  void errorCheck(cudaError_t err, const char* file, int line)
  {
    if( err != cudaSuccess )
    {
      printf("%s(%d): cuda error %d (%s)\n", file, line, err, cudaGetErrorString(err));
      abort();
    }
  }

  //use only for debugging kernels
  //this slows stuff down a LOT
  void syncAndErrorCheck(const char* file, int line)
  {
    cudaThreadSynchronize();
    errorCheck(cudaGetLastError(), file, line);
  }

  //this is undefined at the end of this template definition
  #define CHECK(X) errorCheck(X, __FILE__, __LINE__)
  #define SYNC_CHECK() syncAndErrorCheck(__FILE__, __LINE__)

  template<typename T>
  void gpuAlloc(T* &p, Int n)
  {
//    std::cout << "Allocating " << ((sizeof(T) * n ) >> 20) << " MB" << std::endl;
    if(flag)
      printf("unknown gpu alloc\n");
    CHECK( cudaMalloc(&p, sizeof(T) * n) );
  }

  template<typename T>
  void cpuAlloc(T* &p, Int n)
  {
    p = (T *) malloc(sizeof(T) * n);
  }

  template<typename T>
  void copyToGPU(T* dst, const T* src, Int n)
  {
    CHECK( cudaMemcpy(dst, src, sizeof(T) * n, cudaMemcpyHostToDevice) );
  }

  template<typename T>
  void copyToHost(T* dst, const T* src, Int n)
  {
    //error check please!
    CHECK( cudaMemcpy(dst, src, sizeof(T) * n, cudaMemcpyDeviceToHost) );
  }

  //async copies 
  template<typename T>
  void copyToGPUAsync(T* dst, const T* src, Int n, cudaStream_t str)
  {
    CHECK( cudaMemcpyAsync(dst, src, sizeof(T) * n, cudaMemcpyHostToDevice, str) );
  }

  template<typename T>
  void copyToHostAsync(T* dst, const T* src, Int n, cudaStream_t str)
  {
    //error check please!
    CHECK( cudaMemcpyAsync(dst, src, sizeof(T) * n, cudaMemcpyDeviceToHost, str) );
  }

  void gpuFree(void *ptr)
  {
    if( ptr )
      CHECK( cudaFree(ptr) );
  }

  void cpuFree(void *ptr)
  {
    if( ptr )
      free(ptr);
  }

  dim3 calcGridDim(Int n)
  {
    if (n < 65536)
      return dim3(n, 1, 1);
    else {
      int side1 = static_cast<int>(sqrt((double)n));
      int side2 = static_cast<int>(ceil((double)n / side1));
      return dim3(side2, side1, 1);
    }
  }


  Int divRoundUp(Int x, Int y)
  {
    return (x + y - 1) / y;
  }


  //for debugging
  template<typename T>
  void printGPUArray(T* ptr, int n)
  {
    std::vector<T> tmp(n);
    copyToHost(&tmp[0], ptr, n);
    for( Int i = 0; i < n; ++i )
      std::cout << i << " " << tmp[i] << std::endl;
  }

  //profiling
  //cudaEvent_t m_ev0, m_ev1;

  public:
    GASEngineGPUShard()
      : m_nVertices(0)
//      , m_nEdges(0)
      , m_nCSREdges(0)
      , m_nCSCEdges(0)
      //, m_vertexDataHost(0)
      //, m_edgeDataHost(0)
      , m_vertexDataHost(false)
      , m_edgeDataHost(false)
      , m_vertexData(0)
      , m_edgeData(0)
      , m_srcs(0)
      , m_srcOffsets(0)
      , m_edgeIndexCSC(0)
      , m_dsts(0)
      , m_dstOffsets(0)
      , m_edgeIndexCSR(0)
      , m_active(0)
      , m_nActive(0)
      , m_activeNext(0)
      , m_nActiveNext(0)
      , m_applyRet(0)
//      , m_activeFlags(0)
      , m_edgeCountScan(0)
      , m_gatherMapTmp(0)
//      , m_gatherTmp(0)
      , m_gatherDstsTmp(0)
      //, m_gatherTmpHost(0)
      //, m_edgeOutputCounter(0)
      //, m_outputEdgeList(0)
      , preComputed(false)
    {
      m_mgpuContext = mgpu::CreateCudaDevice(0);
    }

    ~GASEngineGPUShard()
    {
      gpuFree(m_edgeData);
      gpuFree(m_srcs);
      gpuFree(m_srcOffsets);
      gpuFree(m_edgeIndexCSC);
      gpuFree(m_dsts);
      gpuFree(m_dstOffsets);
      gpuFree(m_edgeIndexCSR);
      gpuFree(m_activeNext);
      gpuFree(m_edgeCountScan);
      gpuFree(m_gatherMapTmp);
      gpuFree(m_gatherDstsTmp);
      //cudaFreeHost(m_hostMappedValue);
      //don't we need to explicitly clean up m_mgpuContext?
    }

    //initialize the graph data structures for the GPU
    //All the graph data provided here is "owned" by the GASEngine until
    //explicitly released with getResults().  We may make a copy or we
    //may map directly into host memory
    //The Graph is provided here as an edge list.  We internally convert
    //to CSR/CSC representation.  This separates the implementation details
    //from the vertex program.  Can easily add interfaces for graphs that
    //are already in CSR or CSC format.
    //
    //This function is not optimized and at the moment, this initialization
    //is considered outside the scope of the core work on GAS.
    //We will have to revisit this assumption at some point.
    //NOTE: Changing this function such that only gpuAlloc is being done here. No data transfer is done in this function. This function is called only once by the parent class for each shard just to allocate GPU memory for each shard.
    void setGraph(Int nVertices
//      , VertexData* vertexData
      , bool vertexDataHost
      , Int nEdges
//      , EdgeData* edgeData
      , bool edgeDataHost
//      , const Int *edgeListSrcs
//      , const Int *edgeListDsts
    )      
    {
      m_vertexDataHost = vertexDataHost;
      m_edgeDataHost   = edgeDataHost;

//No need to allocate for vertexData, it is global
/*
      //allocate copy of vertex and edge data on GPU
      if( m_vertexDataHost )
      {
        gpuAlloc(m_vertexData, nVertices);
        //Disabling coptToGPU for now, will be done in another step
        //copyToGPU(m_vertexData, m_vertexDataHost, m_nVertices);
      }
*/

      //allocate CSR and CSC edges
      gpuAlloc(m_srcOffsets, nVertices + 1);
      gpuAlloc(m_dstOffsets, nVertices + 1);
      gpuAlloc(m_srcs, nEdges);
      gpuAlloc(m_dsts, nEdges);

      //These edges are not needed when there is no edge data
      //We only need one of these since we can sort the edgeData directly into
      //either CSR or CSC.  But the memory and performance overhead of these
      //arrays needs to be discussed.
      /*
        sortEdgesForGather is true by default
      */
      if( sortEdgesForGather )
        gpuAlloc(m_edgeIndexCSR, nEdges);
      else
        gpuAlloc(m_edgeIndexCSC, nEdges);

//This code created the CSR and CSC representations which we have done in the parent class.
/*
      //these are pretty big temporaries, but we're assuming 'unlimited'
      //host memory for now.
      std::vector<Int> tmpOffsets(m_nVertices + 1);
      std::vector<Int> tmpVerts(m_nEdges);
      std::vector<Int> tmpEdgeIndex(m_nEdges);
      std::vector<EdgeData> sortedEdgeData(m_nEdges);

      //get CSC representation for gather/apply
      edgeListToCSC(m_nVertices, m_nEdges
        , edgeListSrcs, edgeListDsts
        , &tmpOffsets[0], &tmpVerts[0], &tmpEdgeIndex[0]);

      //sort edge data into CSC order to avoid an indirected read in gather
      if( sortEdgesForGather )
      {
        for(size_t i = 0; i < m_nEdges; ++i)
          sortedEdgeData[i] = m_edgeDataHost[ tmpEdgeIndex[i] ];
      }
      else
        copyToGPU(m_edgeIndexCSC, &tmpEdgeIndex[0], m_nEdges);

      copyToGPU(m_srcOffsets, &tmpOffsets[0], m_nVertices + 1);
      copyToGPU(m_srcs, &tmpVerts[0], m_nEdges);

      //get CSR representation for activate/scatter
      edgeListToCSR(m_nVertices, m_nEdges
        , edgeListSrcs, edgeListDsts
        , &tmpOffsets[0], &tmpVerts[0], &tmpEdgeIndex[0]);

      //sort edge data into CSR order to avoid an indirected write in scatter
      if( !sortEdgesForGather )
      {
        for(size_t i = 0; i < m_nEdges; ++i)
          sortedEdgeData[i] = m_edgeDataHost[ tmpEdgeIndex[i] ];
      }
      else
        copyToGPU(m_edgeIndexCSR, &tmpEdgeIndex[0], m_nEdges);

      copyToGPU(m_dstOffsets, &tmpOffsets[0], m_nVertices + 1);
      copyToGPU(m_dsts, &tmpVerts[0], m_nEdges);
*/

      if( m_edgeDataHost )
      {
        gpuAlloc(m_edgeData, nEdges);
        //Disabling coptToGPU for now, will be done in another step
        //copyToGPU(m_edgeData, &sortedEdgeData[0], m_nEdges);
      }

      //allocate active lists
//      gpuAlloc(m_active, nVertices);
      gpuAlloc(m_activeNext, nVertices);

      //allocate temporaries for current multi-part gather kernels
//      gpuAlloc(m_applyRet, nVertices);
//      gpuAlloc(m_activeFlags, nVertices);
      gpuAlloc(m_edgeCountScan, nVertices);
      //have to allocate extra for faked incoming edges when there are
      //no incoming edges
      gpuAlloc(m_gatherMapTmp, nEdges + nVertices);
      //gpuAlloc(m_gatherTmp, nVertices);
      gpuAlloc(m_gatherDstsTmp, nEdges + nVertices);
      //cpuAlloc(m_gatherTmpHost, nVertices);

      //allocated mapped memory
      cudaMallocHost(&m_hostMappedValue, sizeof(Int), cudaHostAllocMapped );
      cudaHostGetDevicePointer(&m_deviceMappedValue, m_hostMappedValue, 0);

      //allocate for small sized list
      /*gpuAlloc(m_edgeOutputCounter, 1);
      if (m_nEdges < 50000) {
        gpuAlloc(m_outputEdgeList, m_nEdges);
      }
      else {
        gpuAlloc(m_outputEdgeList, m_nEdges / 100 + 1);
      }*/

      cudaEventCreate(&m_ev0);
      cudaEventCreate(&m_ev1);
    }

    //This function copies the data from CPU to GPU for the shard
    void copyGraphIn(Int nVertices
        , VertexData* vertexDataHost
        , Int vertexOffset
//        , Int nEdges
        , Int nCSREdges
        , Int nCSCEdges
        , EdgeData* edgeDataHost
        , const Int *srcsHost
        , const Int *srcOffsetsHost
        , const Int *edgeIndexCSCHost
        , const Int *dstsHost
        , const Int *dstOffsetsHost
        , const Int *edgeIndexCSRHost
        , Int* nActive
        , Int* activeHost
        , Int* applyRetHost
        , char* activeFlagsHost
        , GatherResult *gatherTmpHost
        , bool* preComputedHost
        , cudaStream_t str
        , cudaStream_t *deepCopyStream
        )
    {
      //Copying in the number of vertices and edges in this shard
      m_nVertices  = nVertices;
      m_vertexOffset = vertexOffset;
//      m_nEdges     = nEdges;
      m_nActive    = *nActive;
      m_nActiveShard = nActive;
      m_nCSREdges  = nCSREdges;
      m_nCSCEdges  = nCSCEdges;
      preComputed  = *preComputedHost;
      preComputedShard = preComputedHost;

      //Copying the vertex and edge states in this shard 
      //No need to copy vertexData, it is global, already in GPU, just copy the pointer at the appropriate offset
      if( m_vertexDataHost )
        m_vertexData = vertexDataHost;

#if 0
      if( m_edgeDataHost )
      {
        if( sortEdgesForGather )
          copyToGPU(m_edgeData, edgeDataHost, m_nCSREdges); 
        else
          copyToGPU(m_edgeData, edgeDataHost, m_nCSCEdges); 
      }
#else
      if( m_edgeDataHost )
      {
        if( sortEdgesForGather )
          copyToGPUAsync(m_edgeData, edgeDataHost, m_nCSREdges, deepCopyStream[0]); 
        else
          copyToGPUAsync(m_edgeData, edgeDataHost, m_nCSCEdges, deepCopyStream[0]); 
      }

#endif

      //Copying the CSR and CSC representations of the edges
#if 0
      copyToGPU(m_dsts, dstsHost, m_nCSREdges);
      copyToGPU(m_dstOffsets, dstOffsetsHost, m_nVertices + 1);
      if( sortEdgesForGather )
        copyToGPU(m_edgeIndexCSR, edgeIndexCSRHost, m_nCSREdges);
      copyToGPU(m_srcs, srcsHost, m_nCSCEdges);
      copyToGPU(m_srcOffsets, srcOffsetsHost, m_nVertices + 1);
      if( !sortEdgesForGather )
        copyToGPU(m_edgeIndexCSC, edgeIndexCSCHost, m_nCSCEdges);
#else
      copyToGPUAsync(m_dsts, dstsHost, m_nCSREdges, deepCopyStream[1]);
      copyToGPUAsync(m_dstOffsets, dstOffsetsHost, m_nVertices + 1, deepCopyStream[2]);
      if( sortEdgesForGather )
        copyToGPUAsync(m_edgeIndexCSR, edgeIndexCSRHost, m_nCSREdges, deepCopyStream[3]);
      copyToGPUAsync(m_srcs, srcsHost, m_nCSCEdges, deepCopyStream[4]);
      copyToGPUAsync(m_srcOffsets, srcOffsetsHost, m_nVertices + 1, deepCopyStream[5]);
      if( !sortEdgesForGather )
        copyToGPUAsync(m_edgeIndexCSC, edgeIndexCSCHost, m_nCSCEdges, deepCopyStream[6]);

#endif
      //Copying the active lists
      //copyToGPU(m_active, activeHost, m_nVertices);
      m_active = activeHost;
      //copyToGPU(m_applyRet, applyRetHost, m_nVertices);
      m_applyRet = applyRetHost;
      //copyToGPU(m_activeFlags, activeFlagsHost, m_nVertices);
      m_activeFlags = activeFlagsHost;
      
      m_gatherTmp = gatherTmpHost;

      //TODO: Remove it later
      //CHECK(cudaStreamSynchronize(str));
    }

    //This function copies the data from GPU to CPU for the shard
    void copyGraphOut(EdgeData* edgeDataHost
//        , Int* activeHost
//        , Int* applyRetHost
          , cudaStream_t str
        )
    {
      //Copying the edge states in this shard
#if 0       
      if( m_edgeDataHost )
      {
        if( sortEdgesForGather )
          copyToHost(edgeDataHost, m_edgeData, m_nCSREdges); 
        else
          copyToHost(edgeDataHost, m_edgeData, m_nCSCEdges); 
      }
#else
      if( m_edgeDataHost )
      {
        if( sortEdgesForGather )
          copyToHostAsync(edgeDataHost, m_edgeData, m_nCSREdges, str); 
        else
          copyToHostAsync(edgeDataHost, m_edgeData, m_nCSCEdges, str); 
      }

#endif

      //TODO: Remove it later
      //CHECK(cudaStreamSynchronize(str));

      //The vertex states and active lists are already in GPU memory, so no need to copy back
      //We only need to copy them back to CPU at the end of the runtime
//      copyToHost(activeHost, m_active, m_nVertices);
//      copyToHost(applyRetHost, m_applyRet, m_nVertices);
      //copyToHost(activeFlagsHost, m_activeFlags, m_nVertices);
    }

//We don't need this function anymore in this class, we will move it to the parent class
/*
     //This may be a slow function, so normally would only be called
    //at the end of a computation.  This does not invalidate the
    //data already in the engine, but does make sure that the host
    //data is consistent with the engine's internal data
    void getResults()
    {
      if( m_vertexDataHost )
        copyToHost(m_vertexDataHost, m_vertexData, m_nVertices);
      if( m_edgeDataHost )
      {
        //unsort the edge data - todo
        copyToHost(m_edgeDataHost, m_edgeData, m_nEdges);
      }
    }


    //set the active flag for a range [vertexStart, vertexEnd)
    void setActive(Int vertexStart, Int vertexEnd)
    {
      m_nActive = vertexEnd - vertexStart;
      const int nThreadsPerBlock = 128;
      const int nBlocks = divRoundUp(m_nActive, nThreadsPerBlock);
      dim3 grid = calcGridDim(nBlocks);
      GPUGASKernels::kRange<<<grid, nThreadsPerBlock>>>(vertexStart, vertexEnd, m_active);
    }
*/

    //Return the number of active vertices in the next gather step
    Int countActive()
    {
      return m_nActive;
    }


    //nvcc will not let this struct be private. Why?
    //MGPU-specific, equivalent to a thrust transform_iterator
    //customize scan to use edge counts from either offset differences or
    //a separately stored edge count array given a vertex list
    struct EdgeCountIterator : public std::iterator<std::input_iterator_tag, Int>
    {
      Int *m_offsets;
      Int *m_active;

      __host__ __device__
      EdgeCountIterator(Int *offsets, Int *active) : m_offsets(offsets), m_active(active) {};

      __device__
      Int operator[](Int i) const
      {
        Int active = m_active[i];
        return max(m_offsets[active + 1] - m_offsets[active], 1);
      }

      __device__
      EdgeCountIterator operator +(Int i) const
      {
        return EdgeCountIterator(m_offsets, m_active + i);
      }
    };

    //this one checks if predicate is false and outputs zero if so
    //used in the current impl for scatter, this will go away.
    struct PredicatedEdgeCountIterator : public std::iterator<std::input_iterator_tag, Int>
    {
      Int *m_offsets;
      Int *m_active;
      Int *m_predicates;

      __host__ __device__
      PredicatedEdgeCountIterator(Int *offsets, Int *active, Int * predicates) : m_offsets(offsets), m_active(active), m_predicates(predicates) {};

      __device__
      Int operator[](Int i) const
      {
        Int active = m_active[i];
        return m_predicates[i] ? m_offsets[active + 1] - m_offsets[active] : 0;
      }

      __device__
      PredicatedEdgeCountIterator operator +(Int i) const
      {
        return PredicatedEdgeCountIterator(m_offsets, m_active + i, m_predicates + i);
      }
    };

    //nvcc, why can't this struct by private?
    //wrap Program::gatherReduce for use with thrust
    struct ThrustReduceWrapper : std::binary_function<GatherResult, GatherResult, GatherResult>
    {
      __device__ GatherResult operator()(const GatherResult &left, const GatherResult &right)
      {
        return Program::gatherReduce(left, right);
      }
    };

#if 1
  void printDevice(Int *src, Int n)
  {
    Int * tmpHost;
    cpuAlloc(tmpHost,n);
    copyToHost(tmpHost,src,n);
    for(size_t i= 0; i < n; i++)
          printf("i=%d device val=%d\n",i,tmpHost[i]);
    free(tmpHost);
  }
  void printDeviceFloat(float *src, Int n)
  {
    float * tmpHost;
    cpuAlloc(tmpHost,n);
    copyToHost(tmpHost,src,n);
    for(size_t i= 0; i < n; i++)
          printf("i=%d device val=%f\n",i,tmpHost[i]);
    free(tmpHost);
  }
  void printDeviceChar(char *src, Int n)
  {
    char * tmpHost;
    cpuAlloc(tmpHost,n);
    copyToHost(tmpHost,src,n);
    for(size_t i= 0; i < n; i++)
          printf("i=%d device val=%d\n",i,tmpHost[i]);
    free(tmpHost);
  }

#endif

    void gather(cudaStream_t str, bool haveGather=true)
    {
      if( haveGather )
      {

        //Clearing out the temps
        CHECK( cudaMemset(m_gatherMapTmp, 0, sizeof(GatherResult)*(m_nCSREdges+m_nVertices) )  );
        CHECK( cudaMemset(m_gatherDstsTmp, 0, sizeof(Int)*(m_nCSREdges+m_nVertices) )  );

        //first scan the numbers of edges from the active list
        EdgeCountIterator ecIterator(m_srcOffsets, m_active);
        mgpu::Scan<mgpu::MgpuScanTypeExc, EdgeCountIterator, Int, mgpu::plus<Int>, Int*>(ecIterator
          , m_nActive
          , 0
          , mgpu::plus<int>()
          , m_deviceMappedValue
          , (Int *)NULL
          , m_edgeCountScan
          , *m_mgpuContext);
        cudaDeviceSynchronize();
        const int nThreadsPerBlock = 128;
        Int nActiveEdges = *m_hostMappedValue;

        MGPU_MEM(int) partitions = mgpu::MergePathPartitions<mgpu::MgpuBoundsUpper>
          (mgpu::counting_iterator<int>(0), nActiveEdges, m_edgeCountScan, m_nActive
          , nThreadsPerBlock, 0, mgpu::less<int>(), *m_mgpuContext);

        Int nBlocks = MGPU_DIV_UP(nActiveEdges + m_nActive, nThreadsPerBlock);

        dim3 grid = calcGridDim(nBlocks);
        GPUGASKernels::kGatherMap<Program, Int, nThreadsPerBlock, !sortEdgesForGather>
          <<<grid, nThreadsPerBlock>>>
          ( m_nActive
          , m_active
          , nBlocks
          , nActiveEdges
          , m_edgeCountScan
          , partitions->get()
          , m_srcOffsets
          , m_srcs
          , m_vertexData
          , m_edgeData
          , m_edgeIndexCSC
          , m_vertexOffset
          , m_gatherDstsTmp
          , m_gatherMapTmp );
        //SYNC_CHECK();
#if SYNCD
      print("LINE ");print(__LINE__);
#endif

        {
          mgpu::ReduceByKey(m_gatherDstsTmp
                          , m_gatherMapTmp
                          , nActiveEdges
                          , Program::gatherZero
                          , ThrustReduceWrapper()
                          , mgpu::equal_to<Int>()
                          , (Int *)NULL
                          , m_gatherTmp
                          , NULL
                          , NULL
                          , *m_mgpuContext);
        }
        //SYNC_CHECK();
#if SYNCD
      print("LINE ");print(__LINE__);
#endif

//        copyToHost(m_gatherTmpHost, m_gatherTmp, m_nVertices);
        
      }
    }


    //helper types for scatterActivate that should be private if nvcc would allow it
    //ActivateGatherIterator does an extra dereference: iter[x] = offsets[active[x]]
    struct ActivateGatherIterator : public std::iterator<std::input_iterator_tag, Int>
    {
      Int *m_offsets;
      Int *m_active;

      __host__ __device__
      ActivateGatherIterator(Int* offsets, Int* active)
        : m_offsets(offsets)
        , m_active(active)
      {};

      __device__
      Int operator [](Int i)
      {
        return m_offsets[ m_active[i] ];
      }

      __device__
      ActivateGatherIterator operator +(Int i) const
      {
        return ActivateGatherIterator(m_offsets, m_active + i);
      }
    };

    //"ActivateOutputIterator[i] = dst" effectively does m_flags[dst] = true
    //This does not work because DeviceScatter in moderngpu is written assuming
    //a T* rather than OutputIterator .
    struct ActivateOutputIterator
    {
      char* m_flags;

      __host__ __device__
      ActivateOutputIterator(char* flags) : m_flags(flags) {}

      __device__
      ActivateOutputIterator& operator[](Int i)
      {
        return *this;
      }

      __device__
      void operator =(Int dst)
      {
        m_flags[dst] = true;
      }

      __device__
      ActivateOutputIterator operator +(Int i)
      {
        return ActivateOutputIterator(m_flags);
      }
    };

    struct ActivateOutputIteratorSmallSize
    {
      int* m_count;
      Int* m_list;

      __host__ __device__
      ActivateOutputIteratorSmallSize(int* count, Int *list) : m_count(count), m_list(list) {}

      __device__
      ActivateOutputIteratorSmallSize& operator[](Int i)
      {
        return *this;
      }

      __device__
      void operator =(Int dst)
      {
        int pos = atomicAdd(m_count, 1);
        m_list[pos] = dst;
      }

      __device__
      ActivateOutputIteratorSmallSize operator +(Int i)
      {
        return ActivateOutputIteratorSmallSize(m_count, m_list);
      }
    };

    struct ListToHeadFlagsIterator : public std::iterator<std::input_iterator_tag, Int>
    {
      int *m_list;
      int m_offset;

      __host__ __device__
      ListToHeadFlagsIterator(int *list) : m_list(list), m_offset(0) {}

      __host__ __device__
      ListToHeadFlagsIterator(int *list, int offset) : m_list(list), m_offset(offset) {}

      __device__
      int operator[](int i) {
        if (m_offset == 0 && i == 0)
          return 1;
        else {
          return m_list[m_offset + i] != m_list[m_offset + i - 1];
        }
      }

      __device__
      ListToHeadFlagsIterator operator+(int i) const
      {
        return ListToHeadFlagsIterator(m_list, m_offset + i);
      }
    };

    struct ListOutputIterator : public std::iterator<std::output_iterator_tag, Int>
    {
      int* m_inputlist;
      int* m_outputlist;
      int m_offset;

      __host__ __device__
      ListOutputIterator(int *inputlist, int *outputlist) : m_inputlist(inputlist), m_outputlist(outputlist), m_offset(0) {}

      __host__ __device__
      ListOutputIterator(int *inputlist, int *outputlist, int offset) : m_inputlist(inputlist), m_outputlist(outputlist), m_offset(offset) {}

      __host__ __device__
      ListOutputIterator operator[](Int i) const
      {
        return ListOutputIterator(m_inputlist, m_outputlist, m_offset + i);
      }

      __device__
      void operator =(Int dst)
      {
        if (m_offset == 0) {
          m_outputlist[dst] = m_inputlist[0];
        }
        else {
          if (m_inputlist[m_offset] != m_inputlist[m_offset - 1]) {
            m_outputlist[dst] = m_inputlist[m_offset];
          }
        }
      }

      __device__
      ListOutputIterator operator +(Int i) const
      {
        return ListOutputIterator(m_inputlist, m_outputlist, m_offset + i);
      }
    };

    //not writing a custom kernel for this until we get kGather right because
    //it actually shares a lot with this kernel.
    //this version only does activate, does not actually invoke Program::scatter,
    //this will let us improve the gather kernel and then factor it into something
    //we can use for both gather and scatter.
    void scatterActivate(Int activeOffset, Int gnVertices, cudaStream_t str, bool haveScatter=true)
    {

      //counts = m_applyRet ? outEdgeCount[ m_active ] : 0
      //first scan the numbers of edges from the active list

//TODO: we are memcpying nvertices+1 per shard in copygraphin for dstoffsets, but we shouldn't because we're not changing the indexes
      PredicatedEdgeCountIterator ecIterator(m_dstOffsets, m_active, m_applyRet);
//      printf("before scan : ");print(*m_hostMappedValue);
      mgpu::Scan<mgpu::MgpuScanTypeExc, PredicatedEdgeCountIterator, Int, mgpu::plus<Int>, Int*>(ecIterator
        , m_nActive
        , 0
        , mgpu::plus<Int>()
        , m_deviceMappedValue
        , (Int *)NULL
        , m_edgeCountScan
        , *m_mgpuContext);
      cudaDeviceSynchronize();
  //    printf("before scan : ");print(*m_hostMappedValue);
      Int nActiveEdges = *m_hostMappedValue;
      //SYNC_CHECK();
#if SYNCD
      print("LINE ");print(__LINE__);
#endif

      if (nActiveEdges == 0) {
        m_nActive = 0;
      }
      //100 is an empirically chosen value that seems to give good performance
      //else if (nActiveEdges > m_nEdges / 100) {
      else {
        //Gathers the dst vertex ids from m_dsts and writes a true for each
        //dst vertex into m_activeFlags
//        CHECK( cudaMemset(m_activeFlags + activeOffset, 0, sizeof(char) * m_nVertices) );

        //CHECK( cudaEventRecord(m_ev0) );
        //if(activeOffset == 0)

        /*
         *to find outedges of i'th active vertex
         j=active[i]
         start=m_dstoffset[j]
         end=start+edgecountscan[j]
         for(t=start ; t<end;t++)
           activeflags[dsts[t]]=true;
         * */

        //printf("m_nActive = %d nActiveEdges=%d\n",m_nActive,nActiveEdges);
        IntervalGather(nActiveEdges
          , ActivateGatherIterator(m_dstOffsets, m_active) //m_dstOffsets[m_active[i]] gives global scan offset of m_active[i] vertex
          , m_edgeCountScan
          , m_nActive
          , m_dsts
          , ActivateOutputIterator(m_activeFlags) 
          //TODO: the problem is that this function tries to absolute index m_activeFlags while that is per shard
          //so seg fault occurs
          , *m_mgpuContext);
        //CHECK( cudaEventRecord(m_ev1) ) ;
        //float elapsedTime;
        //CHECK( cudaEventElapsedTime(&elapsedTime, m_ev0, m_ev1) );
        //printf("m_nActive = %d\n, expand took %f ms\n", m_nActive, elapsedTime);
        //cudaThreadSynchronize();
        //SYNC_CHECK();
#if SYNCD
      print("LINE ");print(__LINE__);
#endif
 
        //m_nActive = *m_hostMappedValue;
      }

      //*m_nActiveShard = m_nActive;
    }

};

/*
  Adding a parent class for GASEngineGPUShard.
  Earlier, entire graph was loaded into memory at once. But now, it will
  be loaded in shards. So, we have to pass each shard as an independant
  graph to GASEngineGPUShard. So, the user will make an instance of this parent
  class. Each function will in turn call the corresponding function of
  GASEngineGPUShard for each shard.
*/
template<typename Program
  , typename Int = int32_t
  , bool sortEdgesForGather = true>
class GASEngineGPU
{
public:
  typedef typename Program::VertexData   VertexData;
  typedef typename Program::EdgeData     EdgeData;
  typedef typename Program::GatherResult GatherResult;

private:

  //Defining the default number of CUDA streams i.e. the number of shards in the GPU memory
  static const Int NUM_STREAMS = 2;
  static const Int maxEdgesPerShard = 91042010;//100000000;//91042010;//68993773;//45030389; 
  Int maxVerticesPerShard;
  Int numShards;
  GASEngineGPUShard<Program, Int, sortEdgesForGather> *shard[NUM_STREAMS];
  Int *vertexShardMap;
  Int *edgeShardMapCSR;
  Int *edgeShardMapCSC;
  Int *shardMapTmp;
  Int *nActiveShardMap;

  Int         nVertices;
  Int         nEdges;

  //input/output pointers to host data
  VertexData *vertexDataHost;
  EdgeData   *edgeDataHost;
  bool       vertexDataExist;
  bool       edgeDataExist;

  //GPU copy
  VertexData *vertexData; //on GPU O(V)
  EdgeData   *edgeData; //on CPU O(E)

  //CSC representation for gather phase
  //Kernel accessible data
  Int *srcs; //O(E)
  Int *srcOffsets; //O(V)
  Int *edgeIndexCSC; //O(E)

  //CSR representation for reduce phase
  Int *dsts; //O(E)
  Int *dstOffsets; //O(V)
  Int *edgeIndexCSR; //O(E)

  //Temporary variable needed to sum both in and out edges for the vertices to determine shards
  Int *edgesPerVertexTmpScan;
  Int edgeOffsetTmp;

  //Global active vertex lists
  Int *active; //O(V)
  Int  nActive;
  Int *applyRet; //O(V) //set of vertices whose neighborhood will be active next
  char *activeFlags; //O(V)
  
  GatherResult *gatherTmp;     //store results of gatherReduce()

  //Temporaries kept int GPU memory for correcting the indexes of active list
  Int * v2sMapDevice; //O(V)
  Int * s2vMapDevice; //O(V)
  Int * active2sMapDevice; //O(numShards)
  Int *newActiveTmp;

  bool *preComputed;

  //CUDA Streams
  cudaStream_t * shardStream;
  cudaStream_t * deepCopyStream;

  //MGPU context
  mgpu::ContextPtr mgpuContext;

  //mapped memory to avoid explicit copy of reduced value back to host memory
  Int *hostMappedValue;
  Int *deviceMappedValue;

  //convenience
  void errorCheck(cudaError_t err, const char* file, int line)
  {
    if( err != cudaSuccess )
    {
      printf("%s(%d): cuda error %d (%s)\n", file, line, err, cudaGetErrorString(err));
      abort();
    }
  }

  //use only for debugging kernels
  //this slows stuff down a LOT
  void syncAndErrorCheck(const char* file, int line)
  {
    cudaThreadSynchronize();
    errorCheck(cudaGetLastError(), file, line);
  }

  void syncStreamAndErrorCheck(const char* file, int line)
  {
    cudaThreadSynchronize();
    errorCheck(cudaGetLastError(), file, line);
  }

  public:
    GASEngineGPU()
      : nVertices(0)
      , nEdges(0)
      , vertexDataHost(0)
      , edgeDataHost(0)
      , vertexDataExist(false)
      , edgeDataExist(false)
      , vertexData(0)
      , edgeData(0)
      , srcs(0)
      , srcOffsets(0)
      , edgeIndexCSC(0)
      , dsts(0)
      , dstOffsets(0)
      , edgeIndexCSR(0)
      , active(0)
      , nActive(0)
      , applyRet(0)
      , activeFlags(0)
      , edgesPerVertexTmpScan(0)
      , edgeOffsetTmp(0)
      , numShards(0)
      , vertexShardMap(0)
      , edgeShardMapCSR(0)
      , edgeShardMapCSC(0)
      , shardMapTmp(0)
      , nActiveShardMap(0)
      , maxVerticesPerShard(0)
      , gatherTmp(0)
      , preComputed(0)
    {
      mgpuContext = mgpu::CreateCudaDevice(0);
      for( size_t i = 0; i < NUM_STREAMS; ++i)
        shard[i] = new GASEngineGPUShard<Program, Int, sortEdgesForGather>();
    }
    
    ~GASEngineGPU()
    {
      gpuFree(vertexData);
      cpuFree(edgeData);
      cpuFree(srcs);
      cpuFree(srcOffsets);
      cpuFree(edgeIndexCSC);
      cpuFree(dsts);
      cpuFree(dstOffsets);
      cpuFree(edgeIndexCSR);
      gpuFree(active);
      gpuFree(applyRet);
      gpuFree(activeFlags);
      cpuFree(edgesPerVertexTmpScan);
      cpuFree(vertexShardMap);
      cpuFree(edgeShardMapCSR);
      cpuFree(edgeShardMapCSC);
      //cpuFree(shardMapTmp);
      cpuFree(nActiveShardMap);
      gpuFree(gatherTmp);
      cudaFreeHost(hostMappedValue);
      cpuFree(preComputed);
      gpuFree(s2vMapDevice);
      gpuFree(v2sMapDevice);
      gpuFree(active2sMapDevice);
      gpuFree(newActiveTmp);
  /*    for( size_t i = 0; i < NUM_STREAMS; ++i)
      {
        delete shard[i];
      }*/
    }

  //this is undefined at the end of this template definition
  #define CHECK(X) errorCheck(X, __FILE__, __LINE__)
  #define SYNC_CHECK() syncAndErrorCheck(__FILE__, __LINE__)

  template<typename T>
  void gpuAlloc(T* &p, Int n)
  {
    CHECK( cudaMalloc(&p, sizeof(T) * n) );
  }

  template<typename T>
  void cpuAlloc(T* &p, Int n)
  {
    p = (T *) malloc(sizeof(T) * n);
  }

  void gpuFree(void *ptr)
  {
    if( ptr )
      CHECK( cudaFree(ptr) );
  }

  void cpuFree(void *ptr)
  {
    if( ptr )
      free(ptr);
  }

  dim3 calcGridDim(Int n)
  {
    if (n < 65536)
      return dim3(n, 1, 1);
    else {
      int side1 = static_cast<int>(sqrt((double)n));
      int side2 = static_cast<int>(ceil((double)n / side1));
      return dim3(side2, side1, 1);
    }
  }

  Int divRoundUp(Int x, Int y)
  {
    return (x + y - 1) / y;
  }

  template<typename T>
  void copyToGPU(T* dst, const T* src, Int n)
  {
    CHECK( cudaMemcpy(dst, src, sizeof(T) * n, cudaMemcpyHostToDevice) );
  }

  template<typename T>
  void copyToHost(T* dst, const T* src, Int n)
  {
    //error check please!
    CHECK( cudaMemcpy(dst, src, sizeof(T) * n, cudaMemcpyDeviceToHost) );
  }

  //async copies 
  template<typename T>
  void copyToGPUAsync(T* dst, const T* src, Int n, cudaStream_t str)
  {
    CHECK( cudaMemcpyAsync(dst, src, sizeof(T) * n, cudaMemcpyHostToDevice, str) );
  }

  template<typename T>
  void copyToHostAsync(T* dst, const T* src, Int n, cudaStream_t str)
  {
    //error check please!
    CHECK( cudaMemcpyAsync(dst, src, sizeof(T) * n, cudaMemcpyDeviceToHost, str) );
  }

  template<typename T>
  void copyD2D(T* dst, const T* src, Int n)
  {
    CHECK( cudaMemcpy(dst, src, sizeof(T) * n, cudaMemcpyDeviceToDevice) );
  }

#if 1
  void printDevice(Int *src, Int n)
  {
    Int * tmpHost;
    cpuAlloc(tmpHost,n);
    copyToHost(tmpHost,src,n);
    for(size_t i= 0; i < n; i++)
          printf("i=%d device val=%d\n",i,tmpHost[i]);
    free(tmpHost);
  }
  void printDeviceFloat(float *src, Int n)
  {
    float * tmpHost;
    cpuAlloc(tmpHost,n);
    copyToHost(tmpHost,src,n);
    for(size_t i= 0; i < n; i++)
          printf("i=%d device val=%f\n",i,tmpHost[i]);
    free(tmpHost);
  }
  void printDeviceChar(char *src, Int n)
  {
    char * tmpHost;
    cpuAlloc(tmpHost,n);
    copyToHost(tmpHost,src,n);
    for(size_t i= 0; i < n; i++)
          printf("i=%d device val=%d\n",i,tmpHost[i]);
    free(tmpHost);
  }
  void printVertexData(VertexData *src, Int n)
  {
    VertexData * tmpHost;
    cpuAlloc(tmpHost,n);
    copyToHost(tmpHost,src,n);
    for(size_t i= 0; i < n; i++)
          printf("i=%d device val=%f\n",i,tmpHost[i].rank);
    free(tmpHost);
  }

#endif

  void setGraph(Int u_nVertices //number of vertices
      , VertexData* u_vertexData //vertex states
      , Int u_nEdges //number of edges
      , EdgeData* u_edgeData //edge states
      , const Int *edgeListSrcs //list of src vertices
      , const Int *edgeListDsts) //list of dst vertices
    {
      nVertices  = u_nVertices;
      nEdges     = u_nEdges;
      vertexDataHost = u_vertexData;
      edgeDataHost   = u_edgeData;

      //allocate copy of vertex data on GPU
      if( vertexDataHost )
      {
        gpuAlloc(vertexData, nVertices);
        copyToGPU(vertexData, vertexDataHost, nVertices);
        vertexDataExist = true;
      }

      //allocate CSR and CSC edges
      Int *srcOffsetsTmp, *dstOffsetsTmp;
      cpuAlloc(srcOffsetsTmp, nVertices + 1);
      cpuAlloc(dstOffsetsTmp, nVertices + 1);
      cpuAlloc(srcs, nEdges);
      cpuAlloc(dsts, nEdges);
      cpuAlloc(edgesPerVertexTmpScan, nVertices + 1);

      //These edges are not needed when there is no edge data
      //We only need one of these since we can sort the edgeData directly into
      //either CSR or CSC.  But the memory and performance overhead of these
      //arrays needs to be discussed.
      if( sortEdgesForGather )
        cpuAlloc(edgeIndexCSR, nEdges);
      else
        cpuAlloc(edgeIndexCSC, nEdges);

      if( edgeDataHost )
      {
        cpuAlloc(edgeData, nEdges);
        //copyToGPU(edgeData, &sortedEdgeData[0], nEdges);
        edgeDataExist = true;
      }

      //get CSC representation for gather/apply
      edgeListToCSC(nVertices, nEdges
        , edgeListSrcs, edgeListDsts
        , &srcOffsetsTmp[0], &srcs[0], &edgeIndexCSC[0]);

      //sort edge data into CSC order to avoid an indirected read in gather
      if( sortEdgesForGather )
      {
        for(size_t i = 0; i < nEdges; ++i)
          edgeData[i] = edgeDataHost[ edgeIndexCSC[i] ];
      }

      //get CSR representation for activate/scatter
      edgeListToCSR(nVertices, nEdges
        , edgeListSrcs, edgeListDsts
        , &dstOffsetsTmp[0], &dsts[0], &edgeIndexCSR[0]);

      //sort edge data into CSR order to avoid an indirected write in scatter
      if( !sortEdgesForGather )
      {
        for(size_t i = 0; i < nEdges; ++i)
          edgeData[i] = edgeDataHost[ edgeIndexCSR[i] ];
      }

      //allocate active lists
      gpuAlloc(active, nVertices);
      //allocate temporaries for current multi-part gather kernels
      gpuAlloc(applyRet, nVertices);
      gpuAlloc(activeFlags, nVertices);
      gpuAlloc(newActiveTmp, nVertices);
     
      //allocated mapped memory
      cudaMallocHost(&hostMappedValue, sizeof(Int), cudaHostAllocMapped );
      cudaHostGetDevicePointer(&deviceMappedValue, hostMappedValue, 0);

#if 1
      //Printing GPU device properties
      runTest();
#endif

      //Summing the in and out edges of the vertices for constructing shards
      for(size_t i = 0; i < nVertices + 1 ; ++i)
        edgesPerVertexTmpScan[i] = srcOffsetsTmp[i] + dstOffsetsTmp[i];
      
      size_t i = 0;
      Int *posTmp, pos;
      Int maxEdgesPerShardTmp = maxEdgesPerShard;
      cpuAlloc(shardMapTmp, nVertices);
      
      do
      {
        //Library function to perform binary search greater than equal over the array
        posTmp = std::upper_bound(edgesPerVertexTmpScan, edgesPerVertexTmpScan + nVertices + 1, maxEdgesPerShardTmp);
        pos = posTmp - edgesPerVertexTmpScan;
        
        //Vertices between i and pos belong to numShard
        for(; i < pos; ++i)
          shardMapTmp[i] = numShards;

        numShards++;
        //If edges still left, then next shard can contain upto maxEdgesPerShard number of edges
        maxEdgesPerShardTmp += maxEdgesPerShard;
      }while(posTmp != edgesPerVertexTmpScan + nVertices + 1);

#if 1
      std::cout << numShards << " shards made." << std::endl; 
#endif

      cpuAlloc(preComputed, numShards);
      std::memset(preComputed, 0, sizeof(bool)*numShards);

      cpuAlloc(vertexShardMap, numShards + 1);
      cpuAlloc(edgeShardMapCSR, numShards + 1);
      cpuAlloc(edgeShardMapCSC, numShards + 1);
      cpuAlloc(nActiveShardMap, numShards);
      std::memset(vertexShardMap, 0, sizeof(Int)*(numShards+1));
      std::memset(edgeShardMapCSR, 0, sizeof(Int)*(numShards+1));
      std::memset(edgeShardMapCSC, 0, sizeof(Int)*(numShards+1));
      for(size_t i = 0; i < nVertices; ++i)
      {
        vertexShardMap[shardMapTmp[i] + 1]++;
        edgeShardMapCSC[shardMapTmp[i] + 1] += srcOffsetsTmp[i + 1] - srcOffsetsTmp[i];
        edgeShardMapCSR[shardMapTmp[i] + 1] += dstOffsetsTmp[i + 1] - dstOffsetsTmp[i];
      }

      //Performing scan operation
      for(size_t i = 1; i < numShards + 1; ++i)
      {
        maxVerticesPerShard = (maxVerticesPerShard > vertexShardMap[i]) ? maxVerticesPerShard : vertexShardMap[i];
        vertexShardMap[i] += vertexShardMap[i-1];
        edgeShardMapCSR[i] += edgeShardMapCSR[i-1];
        edgeShardMapCSC[i] += edgeShardMapCSC[i-1];
      }

      cpuAlloc(srcOffsets, nVertices + numShards);
      cpuAlloc(dstOffsets, nVertices + numShards);

      //Fixing indexes of srcOffsets and dstOffsets to local indexes for each shard
      size_t k = 0;
      for(size_t i = 0; i < numShards; ++i)
      {
        srcOffsets[vertexShardMap[i] + i] = 0;
        //dstOffsets[vertexShardMap[i] + i] = dstOffsetsTmp[k];
        dstOffsets[vertexShardMap[i] + i] = 0;
        for(size_t j = vertexShardMap[i]; j < vertexShardMap[i + 1]; ++j)
        {
          srcOffsets[j + i + 1] = srcOffsetsTmp[k + 1] - edgeShardMapCSC[shardMapTmp[k]]; 
          dstOffsets[j + i + 1] = dstOffsetsTmp[k + 1] - edgeShardMapCSR[shardMapTmp[k]]; 
//          dstOffsets[j + i + 1] = dstOffsetsTmp[k + 1]; 
          k++;
        }
      }

      cpuFree(srcOffsetsTmp);
      cpuFree(dstOffsetsTmp);

      gpuAlloc(s2vMapDevice, numShards+1);
      gpuAlloc(v2sMapDevice, nVertices);
      gpuAlloc(active2sMapDevice, numShards);
      copyToGPU(s2vMapDevice, vertexShardMap, numShards+1);
      copyToGPU(v2sMapDevice, shardMapTmp, nVertices);

      // Allocation  and creating CUDA streams for shard movement and execution
      shardStream = (cudaStream_t *) malloc(NUM_STREAMS * sizeof(cudaStream_t)); 
      for(int i = 0; i < NUM_STREAMS; i++)
        CHECK( cudaStreamCreate(&(shardStream[i])) );

        //Creating CUDA streams for deep copies
      deepCopyStream = (cudaStream_t *) malloc(10 * sizeof(cudaStream_t)); 
      for(int i = 0; i < 10; i++)
        CHECK( cudaStreamCreate(&(deepCopyStream[i])) );

      /*
       * Now we know which vertice belongs to which shard
       * We just have to make NUM_STREAMS number of shards, allocate GPU memory and 
       * repeatedly perform memcpy and run the GAS functions
       */
      //Allocating memory for each shard
      for( size_t i = 0; i < NUM_STREAMS; ++i)
        shard[i]->setGraph(maxVerticesPerShard, vertexDataExist, maxEdgesPerShard, edgeDataExist);

      gpuAlloc(gatherTmp, nVertices);
      //print("Alloced here :)");
      flag = 1;
      SYNC_CHECK();
    }

    void runTest()
    {
      int deviceCount;
      CHECK( cudaGetDeviceCount(&deviceCount) );
      if (deviceCount == 0) {
        fprintf(stderr, "error: no devices supporting CUDA.\n");
        exit(EXIT_FAILURE);
      }

      cudaDeviceProp prop;
      int dev = 0;
      CHECK( cudaGetDeviceProperties(&prop, dev) );
      printf("Using device %d:\n", dev);
      printf("%s; global mem: %dB; compute v%d.%d; clock: %d kHz\n",
          prop.name, (int)prop.totalGlobalMem, (int)prop.major, 
          (int)prop.minor, (int)prop.clockRate);
    }

    //set the active flag for a range [vertexStart, vertexEnd)
    void setActive(Int vertexStart, Int vertexEnd)
    {
      nActive = vertexEnd - vertexStart;
      const int nThreadsPerBlock = 128;
      const int nBlocks = divRoundUp(nActive, nThreadsPerBlock);
      dim3 grid = calcGridDim(nBlocks);

      GPUGASKernels::kRange<<<grid, nThreadsPerBlock>>>(vertexStart, vertexEnd, active, s2vMapDevice, v2sMapDevice);
      
      for(size_t i = 0; i < numShards; ++i)
      {
        if(vertexStart >= vertexShardMap[i] && vertexEnd <= vertexShardMap[i+1])
          nActiveShardMap[i] = vertexEnd - vertexStart;
        else if(vertexStart >= vertexShardMap[i] && vertexEnd > vertexShardMap[i+1])
          nActiveShardMap[i] = vertexShardMap[i+1] - vertexStart;
        else if(vertexStart < vertexShardMap[i] && vertexEnd <= vertexShardMap[i+1])
          nActiveShardMap[i] = vertexEnd - vertexShardMap[i];
        else if(vertexStart < vertexShardMap[i] && vertexEnd > vertexShardMap[i+1])
          nActiveShardMap[i] = vertexShardMap[i+1] - vertexShardMap[i];
        else
          nActiveShardMap[i] = 0;
      }
    }

    Int countActive()
    {
      return nActive;
    }

    void gather(bool haveGather=true)
    {

      if( haveGather )
      {

        //Clearing out the temp arrays
        CHECK( cudaMemset(gatherTmp, 0, sizeof(GatherResult)*nVertices) );
 
        //printf("Real active list before gather \n");
        //printDevice(active,nVertices);

        for(size_t i = 0; i < numShards; ++i)
        {
//          printf("i=%d nactiveshard=%d\n",i,nActiveShardMap[i]);
          if(nActiveShardMap[i])
          {
            if( sortEdgesForGather )
              edgeOffsetTmp = edgeShardMapCSR[i];
            else
              edgeOffsetTmp = edgeShardMapCSC[i];

            //Int numShardIndex = (i>0)?nActiveShardMap[i-1]:0;

            shard[i % NUM_STREAMS]->copyGraphIn(
                vertexShardMap[i+1] - vertexShardMap[i]
                , vertexData
                , vertexShardMap[i]
                //, edgeShardMap[i+1] - edgeShardMap[i]
                , edgeShardMapCSR[i+1] - edgeShardMapCSR[i]
                , edgeShardMapCSC[i+1] - edgeShardMapCSC[i]
                , edgeData + edgeOffsetTmp
                , srcs + edgeShardMapCSC[i]
                , srcOffsets + vertexShardMap[i] + i
                , edgeIndexCSC + edgeShardMapCSC[i]
                , dsts + edgeShardMapCSR[i]
                , dstOffsets + vertexShardMap[i] + i
                , edgeIndexCSR + edgeShardMapCSR[i]
                , &nActiveShardMap[i] 
                , active + vertexShardMap[i] 
                //, active + numShardIndex
                , applyRet + vertexShardMap[i] 
                //              , activeFlags + vertexShardMap[i]
                , activeFlags
                , gatherTmp + vertexShardMap[i]
                , &preComputed[i]
                , shardStream[i % NUM_STREAMS]
                , deepCopyStream
                );

            shard[i % NUM_STREAMS ]->gather(shardStream[i % NUM_STREAMS], haveGather);

            //Synchronize current CUDA stream 
            CHECK( cudaStreamSynchronize(shardStream[i % NUM_STREAMS]) );
            /*for(int j = 0; j <=6; j++)
              CHECK( cudaStreamSynchronize(deepCopyStream[j]) );*/

            shard[i % NUM_STREAMS ]->copyGraphOut(
                edgeData + edgeOffsetTmp
                //            , active + vertexShardMap[i] 
                //            , applyRet + vertexShardMap[i] 
                ,shardStream[i % NUM_STREAMS]
                );
          }
        }
      }
    }

    void apply()
    {
      if(nActive)
      {
        const int nThreadsPerBlock = 128;
        const int nBlocks = divRoundUp(nVertices, nThreadsPerBlock);
        dim3 grid = calcGridDim(nBlocks);
          GPUGASKernels::fixRangeOut<<<grid, nThreadsPerBlock>>>(active, nVertices, s2vMapDevice, v2sMapDevice, numShards);
          SYNC_CHECK();
#if SYNCD
          print("LINE ");print(__LINE__);
#endif
          //        m_nActive = *m_hostMappedValue; 

        //copyToGPU(active2sMapDevice, nActiveShardMap, numShards);
        copyToGPUAsync(active2sMapDevice, nActiveShardMap, numShards, 0);
        GPUGASKernels::kApply<Program, Int><<<grid, nThreadsPerBlock>>>
          (nActive, active, gatherTmp, vertexData, applyRet, nVertices, s2vMapDevice, v2sMapDevice, active2sMapDevice, numShards);
        SYNC_CHECK();

#if SYNCD
        print("LINE ");print(__LINE__);
#endif

        GPUGASKernels::fixRangeIn2<<<grid, nThreadsPerBlock>>>(active, nVertices, s2vMapDevice, v2sMapDevice, numShards);
        SYNC_CHECK();
      }
    }

    void scatterActivate(bool haveScatter=true)
    {
      CHECK( cudaMemset(activeFlags, 0, sizeof(char) * nVertices) );
      for(size_t i = 0; i < numShards; ++i)
      {
        if(nActiveShardMap[i])
        {
          //Int numShardIndex = (i>0)?nActiveShardMap[i-1]:0;
          if( sortEdgesForGather )
            edgeOffsetTmp = edgeShardMapCSR[i];
          else
            edgeOffsetTmp = edgeShardMapCSC[i];

          shard[i % NUM_STREAMS ]->copyGraphIn(
              vertexShardMap[i+1] - vertexShardMap[i]
              , vertexData
              , vertexShardMap[i]
              //            , edgeShardMap[i+1] - edgeShardMap[i]
              , edgeShardMapCSR[i+1] - edgeShardMapCSR[i]
              , edgeShardMapCSC[i+1] - edgeShardMapCSC[i]
              , edgeData + edgeOffsetTmp
              , srcs + edgeShardMapCSC[i]
              , srcOffsets + vertexShardMap[i] + i
              , edgeIndexCSC + edgeShardMapCSC[i]
              , dsts + edgeShardMapCSR[i]
              , dstOffsets + vertexShardMap[i] + i
              , edgeIndexCSR + edgeShardMapCSR[i]
              , &nActiveShardMap[i] 
              , active + vertexShardMap[i] 
              //, active + numShardIndex 
              , applyRet + vertexShardMap[i] 
              //            , activeFlags + vertexShardMap[i] 
              , activeFlags
              , gatherTmp + vertexShardMap[i]
              , &preComputed[i]
              , shardStream[i % NUM_STREAMS]
              , deepCopyStream
              );

          shard[i % NUM_STREAMS ]->scatterActivate(vertexShardMap[i], nVertices, shardStream[i % NUM_STREAMS], haveScatter);

          //Synchronize current CUDA stream 
          CHECK( cudaStreamSynchronize(shardStream[i % NUM_STREAMS]) );
          /*for(int j = 0; j <=6; j++)
            CHECK( cudaStreamSynchronize(deepCopyStream[j]) );*/

          //       std::cout << "reached here" << std::endl; 
          //  nActiveShardMap[i] = shard[i % NUM_STREAMS ]->countActive();
          shard[i % NUM_STREAMS ]->copyGraphOut(
              edgeData + edgeOffsetTmp
              //            , active + vertexShardMap[i] 
              //            , applyRet + vertexShardMap[i] 
              ,shardStream[i % NUM_STREAMS]
              );
        }
      }

      {
        //convert m_activeFlags to new active compact list in m_active
       //set m_nActive to the number of active vertices
       
      for(size_t i = 0; i < numShards; ++i)
      {
        int num = vertexShardMap[i+1] - vertexShardMap[i];
        MGPU_MEM(int) dum_map = mgpuContext->Malloc<int>(num);
        mgpu::Scan<mgpu::MgpuScanTypeExc>(activeFlags + vertexShardMap[i]
            , num
            , 0
            , mgpu::plus<int>()
            , deviceMappedValue
            , (int *)NULL
            , dum_map->get()
            , *mgpuContext);
        SYNC_CHECK();
#if SYNCD
        print("LINE ");print(__LINE__);
#endif
        nActiveShardMap[i] = *hostMappedValue; 
      }

        scatter_if_inputloc_twophase(nVertices,
                                     activeFlags,
                                     active,
                                     deviceMappedValue,
                                     mgpuContext);
       SYNC_CHECK();
       nActive = *hostMappedValue;

         
       if( nActive )
       {
         const int nThreadsPerBlock = 128;
         const int nBlocks = divRoundUp(nActive, nThreadsPerBlock);
         dim3 grid = calcGridDim(nBlocks);
     
         //copyToGPU(active2sMapDevice, nActiveShardMap, numShards);
         copyToGPUAsync(active2sMapDevice, nActiveShardMap, numShards, 0);
         GPUGASKernels::fixRangeIn<<<grid, nThreadsPerBlock>>>(newActiveTmp, active, nActive, s2vMapDevice, v2sMapDevice, active2sMapDevice, numShards);
         SYNC_CHECK();
         copyD2D(active, newActiveTmp, nVertices);

#if SYNCD
         print("LINE ");print(__LINE__);
#endif
       }

      }
      //printf("AFTER INPUTLOC\n");
      //printDeviceChar(activeFlags, nVertices);
    }

    Int nextIter()
    {
/*      Int nActiveNext = 0;
      for(size_t i = 0; i < numShards; ++i)
      {
        nActiveNext += nActiveShardMap[i];
      }
      nActive = nActiveNext;
      std::cout << "Iteration completed." << std::endl;
      */
      return nActive;
    }

    //This may be a slow function, so normally would only be called
    //at the end of a computation.  This does not invalidate the
    //data already in the engine, but does make sure that the host
    //data is consistent with the engine's internal data
    void getResults()
    {
#if 0
      printf("Printing final vertexData\n");
      printVertexData(vertexData,nVertices);
      if( vertexDataExist )
        copyToHost(vertexDataHost, vertexData, nVertices);
#endif

    }

    //single entry point for the whole affair, like before.
    //special cases that don't need gather or scatter can
    //easily roll their own loop.
    void run()
    {
      int i=1;
      while( countActive() )
      {
#if VERBOSE
        printf("Iteration %d nActive %d\n",i,nActive);
#endif
        gather();
        apply();
        scatterActivate();
        nextIter();
        i++;
      }
      printf("Iterations: %d\n", i-1);
    }

};

#endif
