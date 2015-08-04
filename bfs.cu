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

//BFS using vertexAPI2

#include "util.cuh"
#include "graphio.h"
#include "refgas.h"
#include "gpugas.h"


//nvcc doesn't like the __device__ variable to be a static member inside BFS
//so these are both outside.
int g_iterationCount;
__device__ __constant__ int g_iterationCountGPU;


struct BFS
{
  struct VertexData
  {
    int depth;
  };

  struct EdgeData {}; //nothing

  typedef int GatherResult;
  static const int gatherZero = INT_MAX - 1;

  __host__ __device__
  static int gatherReduce(const int& left, const int& right)
  {
    return 0; //do nothing
  }


  __host__ __device__
  static int gatherMap(
    const VertexData* dst, const VertexData *src, const EdgeData* edge)
  {
    return 0; //do nothing
  }


  __host__ __device__
  static bool apply(VertexData* vert, int dist)
  {
    if( vert->depth == -1 )
    {
      #ifdef __CUDA_ARCH__
        vert->depth = g_iterationCountGPU;
      #else
        vert->depth = g_iterationCount;
      #endif
      return true;
    }
    return false;
  }


  __host__ __device__
  static void scatter(
    const VertexData* src, const VertexData *dst, EdgeData* edge)
  {
    //nothing
  }
};


template<bool GPU>
void setIterationCount(int v)
{
  if( GPU )
    cudaMemcpyToSymbol(g_iterationCountGPU, &v, sizeof(v));
  else
    g_iterationCount = v;
}


template<typename Engine, bool GPU>
float run(int nVertices, BFS::VertexData* vertexData, int nEdges
  , const int *srcs, const int *dsts, int sourceVertex)
{
  Engine engine;
  int iteration;

  GpuTimer gpu_timer;
  float elapsed = 0.0f;

  // average elapsed time of 10 runs
  for (int itr = 0; itr < 1; ++itr)
  {
    // reset the graph
    for(int i = 0; i < nVertices; ++i) vertexData[i].depth = -1;
    engine.setGraph(nVertices, vertexData, nEdges, 0, &srcs[0], &dsts[0]);
    engine.setActive(sourceVertex, sourceVertex+1);
    iteration = 0;
    setIterationCount<GPU>(iteration);

    gpu_timer.Start();

    while( engine.countActive() )
    {
      //run apply without gather
      engine.gather(false);
      engine.apply();
      engine.scatterActivate(false);
      engine.nextIter();
      setIterationCount<GPU>(++iteration);
    }
    engine.getResults();

    gpu_timer.Stop();
    elapsed += gpu_timer.ElapsedMillis();
  }

  elapsed /= 1;
  // printf("Took %f ms\n", elapsed);
  printf("search depth (number of iterations): %d\n", iteration);
  return elapsed;
}


void outputDepths(int nVertices, BFS::VertexData* vertexData, FILE *f = stdout)
{
  for( int i = 0; i < nVertices; ++i )
    fprintf(f, "%d %d\n", i, vertexData[i].depth);
}


int main(int argc, char** argv)
{
  char *inputFilename;
  char *outputFilename = 0;
  int sourceVertex;
  bool runTest;
  bool dumpResults;
  bool useMaxOutDegreeStart;
  if(!parseCmdLineSimple(argc, argv, "si-t-d-m|s", &inputFilename, &sourceVertex
    , &runTest, &dumpResults, &useMaxOutDegreeStart, &outputFilename) )
  {
    printf("Usage: bfs [-t] [-d] [-m] inputfile source [outputFilename]\n");
    exit(1);
  }

  //load the graph
  int nVertices;
  std::vector<int> srcs;
  std::vector<int> dsts;
  loadGraph(inputFilename, nVertices, srcs, dsts);

  //initialize vertex data
  std::vector<BFS::VertexData> vertexData(nVertices);
  for( int i = 0; i < nVertices; ++i )
    vertexData[i].depth = -1;

/*
  useMaxOutDegreeStart is a boolean which indicates whether to use 
  the start vertex to be the default i.e. 1 or the vertex with the
  maximum out degree.
*/
  if( useMaxOutDegreeStart )
  {
    //convert to CSR layout to find source vertex
    std::vector<int> srcOffsets(nVertices + 1);
    std::vector<int> csrSrcs(srcs.size());
    edgeListToCSR<int>(
      nVertices, srcs.size(), &srcs[0], &dsts[0], &srcOffsets[0], 0, 0);
    int maxDegree = -1;
    sourceVertex = -1;
    for(int i = 0; i < nVertices; ++i)
    {
      int outDegree = srcOffsets[i + 1] - srcOffsets[i];
      if( outDegree > maxDegree )
      {
        maxDegree    = outDegree;
        sourceVertex = i;
      }
    }
    printf(
      "using vertex %d with degree %d as source\n", sourceVertex, maxDegree);
  }

  std::vector<BFS::VertexData> refVertexData;
  if( runTest )
  {
    refVertexData = vertexData;
    float elapsed = run<GASEngineRef<BFS>, false>(nVertices
      , &refVertexData[0], (int)srcs.size(), &srcs[0], &dsts[0], sourceVertex);
    if( dumpResults )
    {
      printf("Reference:\n");
      outputDepths(nVertices, &refVertexData[0]);
    }
  }

/*
  The following line calls the run() function which is the main
  function that executes the graph algorithm.
  true indicates that the code is to be run on GPU.
  false indicates that the code is to be run on CPU.
*/
  float elapsed = run<GASEngineGPU<BFS>, true>(nVertices, &vertexData[0]
    , (int) srcs.size(), &srcs[0], &dsts[0], sourceVertex);

  // compute stats
  int nodes_visited = 0;
  int edges_visited = 0;
  std::vector<int> srcOffsets(nVertices + 1);
  std::vector<int> csrSrcs(srcs.size());
  edgeListToCSR<int>(
    nVertices, srcs.size(), &srcs[0], &dsts[0], &srcOffsets[0], 0, 0);

  for (int itr = 0; itr < nVertices; ++itr)
  {
    if (vertexData[itr].depth > -1)
    {
      nodes_visited += 1;
      edges_visited += srcOffsets.at(itr+1) - srcOffsets.at(itr);
    }
  }

  printf("nodes visited: %d edges visited: %d\n", nodes_visited, edges_visited);
  float m_teps = (float) edges_visited / (elapsed * 1000);
  printf("elapsed: %.4f ms, MTEPS: %.4f MiEdges/s\n", elapsed, m_teps);

  if( dumpResults )
  {
    printf("GPU:\n");
    outputDepths(nVertices, &vertexData[0]);
  }

  if( runTest )
  {
    bool diff = false;
    for( int i = 0; i < nVertices; ++i )
    {
      if( vertexData[i].depth != refVertexData[i].depth )
      {
        printf("%d %d %d\n", i, refVertexData[i].depth, vertexData[i].depth);
        diff = true;
      }
    }
    if( diff )
      return 1;
    else
      printf("No differences found\n");
  }

  if( outputFilename )
  {
    printf("writing results to %s\n", outputFilename);
    FILE* f = fopen(outputFilename, "w");
    outputDepths(nVertices, &vertexData[0], f);
    fclose(f);
  }

  free(inputFilename);
  free(outputFilename);

  return 0;
}
