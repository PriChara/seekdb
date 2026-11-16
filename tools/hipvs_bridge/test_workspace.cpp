// CPU contract test only: this does not measure GPU speed or ANN recall.
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>

enum { CUVS_SUCCESS = 0, kDLCPU = 1, kDLCUDA = 2, kDLFloat = 2,
       kDLUInt = 1, NN_DESCENT = 1, NO_FILTER = 0, cudaSuccess = 0,
       cudaMemcpyDefault = 0 };
using cuvsError_t = int;
using cuvsResources_t = int;
struct Params { int build_algo, graph_degree, intermediate_graph_degree, nn_descent_niter; };
using cuvsCagraIndexParams_t = Params*;
struct Index { long n, dim; };
using cuvsCagraIndex_t = Index*;
struct SearchParams { size_t itopk_size; int search_width; };
using cuvsCagraSearchParams_t = SearchParams*;
struct Device { int device_type, device_id; };
struct Dtype { int code, bits, lanes; };
struct Tensor { void* data; Device device; int ndim; Dtype dtype;
                long* shape; long* strides; size_t byte_offset; };
using DLTensor = Tensor;
struct DLManagedTensor { Tensor dl_tensor; void (*deleter)(DLManagedTensor*); };
struct cuvsFilter { int type; uintptr_t addr; };
static std::set<void*> allocations;
static int alloc_calls, parameter_calls, fail_alloc;
static size_t observed_itopk;
static int cuvsResourcesCreate(int* r) { *r=1; return 0; }
static int cuvsResourcesDestroy(int) { return 0; }
static const char* cuvsGetLastErrorText() { return "injected test error"; }
static int cuvsCagraIndexParamsCreate(Params** p) { *p=new Params{}; return 0; }
static int cuvsCagraIndexParamsDestroy(Params* p) { delete p; return 0; }
static int cuvsCagraIndexCreate(Index** p) { *p=new Index{}; return 0; }
static int cuvsCagraIndexDestroy(Index* p) { delete p; return 0; }
static int cuvsCagraBuild(int,Params* params,DLManagedTensor* t,Index* i) {
  assert(params->build_algo==NN_DESCENT && params->graph_degree==128);
  assert(params->intermediate_graph_degree==160 && params->nn_descent_niter==72);
  i->n=t->dl_tensor.shape[0]; i->dim=t->dl_tensor.shape[1]; return 0;
}
static int cuvsRMMAlloc(int,void** p,size_t n) {
  ++alloc_calls;
  if (fail_alloc) { --fail_alloc; return 1; }
  *p=std::malloc(n); if (!*p) return 1;
  assert(allocations.insert(*p).second); return 0;
}
static int cuvsRMMFree(int,void* p,size_t) {
  assert(allocations.erase(p)==1); std::free(p); return 0;
}
static int cudaMemcpy(void* dst,const void* src,size_t n,int) {
  std::memcpy(dst,src,n); return 0;
}
static int cudaMemcpy2D(void* dst,size_t dpitch,const void* src,size_t spitch,
                        size_t width,size_t height,int) {
  for (size_t row=0;row<height;++row) {
    std::memcpy(static_cast<char*>(dst)+row*dpitch,static_cast<const char*>(src)+row*spitch,width);
  }
  return 0;
}
static int cuvsCagraSearchParamsCreate(SearchParams** p) {
  ++parameter_calls; *p=new SearchParams{64,1}; return 0;
}
static int cuvsCagraSearchParamsDestroy(SearchParams* p) { delete p; return 0; }
static int cuvsCagraSearch(int,SearchParams* p,Index*,DLManagedTensor* q,
                          DLManagedTensor* ids,DLManagedTensor* distances,cuvsFilter) {
  observed_itopk=p->itopk_size;
  auto count=q->dl_tensor.shape[0]*ids->dl_tensor.shape[1];
  for (long j=0;j<count;++j) {
    static_cast<unsigned*>(ids->dl_tensor.data)[j]=static_cast<unsigned>(j%10);
    static_cast<float*>(distances->dl_tensor.data)[j]=float(j%10);
  }
  return 0;
}
static int cuvsCagraSerialize(int,const char*,Index*,bool) { return 0; }
static int cuvsCagraDeserialize(int,const char*,Index* i) { i->n=1000;i->dim=2;return 0; }
static int cuvsCagraIndexGetSize(Index* i,int64_t* n) { *n=i->n;return 0; }
static int cuvsCagraIndexGetDims(Index* i,int64_t* d) { *d=i->dim;return 0; }
static int dataset_views_released;
static bool invalid_dataset_stride;
static int cuvsCagraIndexGetDataset(Index* i,DLManagedTensor* view) {
  static float padded[4001]={};
  static long shape[2], strides[2]={4,1};
  shape[0]=i->n; shape[1]=i->dim;
  strides[1]=invalid_dataset_stride ? 2 : 1;
  for (long row=0;row<i->n;++row) {
    padded[1+row*4]=float(row);
    padded[2+row*4]=float(-row);
  }
  view->dl_tensor={padded,{kDLCUDA,0},2,{kDLFloat,32,1},shape,strides,sizeof(float)};
  view->deleter=[](DLManagedTensor*) { ++dataset_views_released; };
  return 0;
}

#include "seekdb_cuvs_bridge.c"

int main() {
  float base[2000]={}; float query[8]={}; unsigned ids[800]={}; float dist[800]={};
  void* h=seekdb_cuvs_build(base,1000,2); assert(h);
  assert(seekdb_cuvs_search(h,query,1,10,ids,dist)==0);
  assert(alloc_calls==3 && parameter_calls==1 && ids[9]==9 && dist[9]==9);
  for (int i=0;i<5;++i) assert(seekdb_cuvs_search(h,query,1,10,ids,dist)==0);
  assert(alloc_calls==3 && parameter_calls==1); // warm requests allocate nothing
  assert(seekdb_cuvs_search(h,query,4,200,ids,dist)==0);
  assert(observed_itopk==800);
  int warm=alloc_calls;
  assert(seekdb_cuvs_search(h,query,1,10,ids,nullptr)==0);
  assert(alloc_calls==warm && observed_itopk==512); // no stale larger parameters
  fail_alloc=1;
  assert(seekdb_cuvs_search(h,query,5,10,ids,dist)==10);
  assert(seekdb_cuvs_search(h,query,1,10,ids,dist)==0); // failure keeps old workspace
  assert(seekdb_cuvs_search(h,query,0,10,ids,dist)!=0);
  assert(seekdb_cuvs_search(h,query,1,1001,ids,dist)!=0);
  assert(seekdb_cuvs_search(h,query,LONG_MAX,10,ids,dist)!=0);
  seekdb_cuvs_free(h); assert(allocations.empty());
  h=seekdb_cuvs_deserialize("unused",1000,2); assert(h);
  assert(seekdb_cuvs_search(h,query,1,10,ids,dist)==0);
  float recovered[2000];
  assert(seekdb_cuvs_copy_vectors(h,recovered,1000,2)==0);
  for (int row=0;row<1000;++row) {
    assert(recovered[2*row]==row && recovered[2*row+1]==-row);
  }
  assert(dataset_views_released==1);
  assert(seekdb_cuvs_copy_vectors(h,recovered,999,2)!=0);
  invalid_dataset_stride=true;
  assert(seekdb_cuvs_copy_vectors(h,recovered,1000,2)!=0 && dataset_views_released==2);
  seekdb_cuvs_free(h); assert(allocations.empty());
  std::puts("CPU contract checks passed (not a GPU benchmark)");
}
