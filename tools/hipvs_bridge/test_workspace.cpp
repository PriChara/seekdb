// CPU contract test only: this does not measure GPU speed or ANN recall.
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>

enum { CUVS_SUCCESS = 0, kDLCPU = 1, kDLCUDA = 2, kDLFloat = 2,
       kDLUInt = 1, NN_DESCENT = 1, IVF_PQ = 2, NO_FILTER = 0, cudaSuccess = 0,
       cudaMemcpyDefault = 0 };
using cuvsError_t = int;
using cuvsResources_t = int;
struct IvfBuildParams { int n_lists, pq_dim; };
struct IvfSearchParams { int n_probes; };
struct cuvsIvfPqParams {
  IvfBuildParams* ivf_pq_build_params;
  IvfSearchParams* ivf_pq_search_params;
  float refinement_rate;
};
struct Params {
  int build_algo, graph_degree, intermediate_graph_degree, nn_descent_niter;
  cuvsIvfPqParams* graph_build_params;
};
using cuvsCagraIndexParams_t = Params*;
struct Index { long n, dim; };
using cuvsCagraIndex_t = Index*;
struct SearchParams { size_t itopk_size; int search_width; };
using cuvsCagraSearchParams_t = SearchParams*;
struct Device { int device_type, device_id; };
struct Dtype { int code, bits, lanes; };
struct Tensor { void* data; Device device; int ndim; Dtype dtype;
                long* shape; long* strides; size_t byte_offset; };
struct DLManagedTensor { Tensor dl_tensor; };
struct cuvsFilter { int type; uintptr_t addr; };
static std::set<void*> allocations;
static int alloc_calls, parameter_calls, fail_alloc;
static int ivf_builds, nn_builds;
static bool fail_ivf;
static size_t observed_itopk;
static int cuvsResourcesCreate(int* r) { *r=1; return 0; }
static int cuvsResourcesDestroy(int) { return 0; }
static const char* cuvsGetLastErrorText() { return "injected test error"; }
static int cuvsCagraIndexParamsCreate(Params** p) { *p=new Params{}; return 0; }
static int cuvsCagraIndexParamsDestroy(Params* p) { delete p; return 0; }
static int cuvsCagraIndexCreate(Index** p) { *p=new Index{}; return 0; }
static int cuvsCagraIndexDestroy(Index* p) { delete p; return 0; }
static int cuvsIvfPqIndexParamsCreate(IvfBuildParams** p) { *p=new IvfBuildParams{}; return 0; }
static int cuvsIvfPqIndexParamsDestroy(IvfBuildParams* p) { delete p; return 0; }
static int cuvsIvfPqSearchParamsCreate(IvfSearchParams** p) { *p=new IvfSearchParams{}; return 0; }
static int cuvsIvfPqSearchParamsDestroy(IvfSearchParams* p) { delete p; return 0; }
static int cuvsCagraBuild(int,Params* params,DLManagedTensor* t,Index* i) {
  assert(params->graph_degree==128);
  assert(params->nn_descent_niter==64);
  assert(i->n==0); // An unsuccessful IVF build must not be retried on its handle.
  if (params->build_algo==IVF_PQ) {
    ++ivf_builds;
    assert(params->intermediate_graph_degree==256);
    assert(params->graph_build_params && params->graph_build_params->refinement_rate==2.0f);
    if (fail_ivf) { i->n=-1; return 1; }
  } else {
    ++nn_builds;
    assert(params->intermediate_graph_degree==160);
    assert(params->build_algo==NN_DESCENT && !params->graph_build_params);
  }
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
  seekdb_cuvs_free(h); assert(allocations.empty());
  const int previous_nn_builds=nn_builds;
  h=seekdb_cuvs_build(base,50000,256); assert(h); // The mock does not read the dataset.
  assert(ivf_builds==1 && nn_builds==previous_nn_builds);
  seekdb_cuvs_free(h);
  fail_ivf=true;
  h=seekdb_cuvs_build(base,50000,256); assert(h);
  assert(ivf_builds==2 && nn_builds==previous_nn_builds+1);
  seekdb_cuvs_free(h);
  std::puts("CPU contract checks passed (not a GPU benchmark)");
}
