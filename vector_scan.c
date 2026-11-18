#include <float.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

typedef double (*distance_fn)(const float *, const double *, size_t);

static double squared_distance(const float *row, const double *probe, size_t dim)
{
    double sum = 0;
    for (size_t i = 0; i < dim; ++i) {
        double delta = row[i] - probe[i];
        sum += delta * delta;
    }
    return sum;
}

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2,fma")))
static double squared_distance_avx2(const float *row, const double *probe, size_t dim)
{
    __m256d acc[4] = {
        _mm256_setzero_pd(), _mm256_setzero_pd(),
        _mm256_setzero_pd(), _mm256_setzero_pd()
    };
    size_t i = 0;
    for (; dim - i >= 16; i += 16) {
        for (size_t lane = 0; lane < 4; ++lane) {
            size_t offset = i + 4 * lane;
            __m256d values = _mm256_cvtps_pd(_mm_loadu_ps(row + offset));
            __m256d delta = _mm256_sub_pd(values, _mm256_loadu_pd(probe + offset));
            acc[lane] = _mm256_fmadd_pd(delta, delta, acc[lane]);
        }
    }
    __m256d combined = _mm256_add_pd(_mm256_add_pd(acc[0], acc[1]),
                                     _mm256_add_pd(acc[2], acc[3]));
    double lanes[4];
    _mm256_storeu_pd(lanes, combined);
    return (lanes[0] + lanes[1]) + (lanes[2] + lanes[3])
        + squared_distance(row + i, probe + i, dim - i);
}
#endif

/* Replace the root of a max heap, or heapify it from an internal node. */
static void sift_down(double *heap, size_t size, size_t pos, double value)
{
    while (pos < size / 2) {
        size_t child = pos * 2 + 1;
        if (child + 1 < size && heap[child + 1] > heap[child])
            ++child;
        if (heap[child] <= value)
            break;
        heap[pos] = heap[child];
        pos = child;
    }
    heap[pos] = value;
}

size_t vector_shortlist(const float *rows, const double *probe, size_t dim,
                        const uint32_t *positions, size_t count, size_t k,
                        double *distances, double *heap, uint32_t *candidates)
{
    distance_fn distance = squared_distance;
#if defined(__x86_64__) && defined(__GNUC__)
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
        distance = squared_distance_avx2;
#endif
    for (size_t i = 0; i < count; ++i) {
        size_t row = positions ? positions[i] : i;
        distances[i] = distance(rows + row * dim, probe, dim);
    }
    for (size_t i = 0; i < k; ++i)
        heap[i] = distances[i];
    for (size_t i = k / 2; i > 0; --i)
        sift_down(heap, k, i - 1, heap[i - 1]);
    for (size_t i = k; i < count; ++i) {
        if (distances[i] < heap[0])
            sift_down(heap, k, 0, distances[i]);
    }

    /* Float32 differences and squares stay in the normal double range.
       Nonnegative summation has an O(dim * eps) relative error. Include
       a margin for both distances before the caller recomputes exact ranks. */
    double cutoff = heap[0] * (1.0 + (4.0 * dim + 16.0) * DBL_EPSILON);
    size_t size = 0;
    for (size_t i = 0; i < count; ++i) {
        if (distances[i] <= cutoff)
            candidates[size++] = positions ? positions[i] : (uint32_t)i;
    }
    return size;
}
