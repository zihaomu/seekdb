/* Standalone CAGRA build/recall bench for the contest tuning loop.
 *
 * Why this exists: the official flow needs a full seekdb build (up to 2h) and a
 * 15-minute GPU evaluation just to learn one (time, recall) pair. Tuning a build
 * graph that way costs one 4-hour instance per few data points. This program links
 * the same bridge the server links, so a single compile yields a sweep over build
 * parameters in seconds per point.
 *
 * It is a tuning aid only. Nothing here is submitted as a performance claim; the
 * official score comes from the organiser's harness alone.
 *
 *   ./bench_cagra 250000 128 200 100
 *   OB_CAGRA_NN_ITER=40 ./bench_cagra 250000 128 200 100
 *
 * Reports: build wall time, Recall@10, Recall@100 against exact L2 ground truth.
 */

#include "seekdb_cuvs_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_seconds(void)
{
  /* Same portability reasoning as the bridge: timespec_get is C11, clock() is
   * C89, and clock_gettime is POSIX and disappears under a strict -std. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
  return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

/* Deterministic, allocation-free PRNG: identical data across runs and configs is
 * the whole point, otherwise two configs are not comparable. */
static uint64_t g_rng = 88172645463325252ULL;

static float next_unit(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (float)((g_rng >> 11) * (1.0 / 9007199254740992.0));
}

typedef struct {
  float dist;
  int32_t index;
} pair_t;

/* Keep only the best `capacity` rows, in ascending distance order. A full qsort of
 * 250k rows per query costs tens of seconds and hides the effect being measured;
 * this pass is linear and only touches the array when a row beats the worst kept. */
static void keep_best(pair_t *best, long *count, long capacity, float dist, int32_t index)
{
  if (*count == capacity && dist >= best[capacity - 1].dist) return;
  long pos = (*count < capacity) ? *count : capacity - 1;
  while (pos > 0 && best[pos - 1].dist > dist) {
    best[pos] = best[pos - 1];
    --pos;
  }
  best[pos].dist = dist;
  best[pos].index = index;
  if (*count < capacity) ++*count;
}

int main(int argc, char **argv)
{
  if (argc < 5) {
    fprintf(stderr, "usage: %s <n> <dim> <nq> <topk>\n", argv[0]);
    return 2;
  }
  const long n = atol(argv[1]);
  const long dim = atol(argv[2]);
  const long nq = atol(argv[3]);
  const long topk = atol(argv[4]);
  if (n <= 0 || dim <= 0 || nq <= 0 || topk <= 0 || topk > n) {
    fprintf(stderr, "invalid dimensions\n");
    return 2;
  }
  const long k10 = topk < 10 ? topk : 10;

  float *base = (float *)malloc(sizeof(float) * n * dim);
  float *queries = (float *)malloc(sizeof(float) * nq * dim);
  int32_t *got = (int32_t *)malloc(sizeof(int32_t) * nq * topk);
  if (base == NULL || queries == NULL || got == NULL) {
    fprintf(stderr, "out of host memory\n");
    return 3;
  }
  for (long i = 0; i < n * dim; ++i) base[i] = next_unit() * 2.0f - 1.0f;
  /* Queries are drawn from the same distribution as the base: an out-of-distribution
   * query set would overstate every bit of recall. */
  for (long i = 0; i < nq * dim; ++i) queries[i] = next_unit() * 2.0f - 1.0f;

  /* ---- GPU build under test ---------------------------------------------- */
  const double build_start = now_seconds();
  void *handle = seekdb_cuvs_build(base, n, dim);
  const double build_seconds = now_seconds() - build_start;
  if (handle == NULL) {
    fprintf(stderr, "build failed\n");
    return 4;
  }

  /* ---- GPU search -------------------------------------------------------- */
  const double search_start = now_seconds();
  const int rc = seekdb_cuvs_search(handle, queries, nq, topk, (unsigned int *)got, NULL);
  const double search_seconds = now_seconds() - search_start;
  if (rc != 0) {
    fprintf(stderr, "search failed rc=%d\n", rc);
    seekdb_cuvs_free(handle);
    return 5;
  }

  /* ---- exact ground truth, CPU ------------------------------------------ */
  pair_t *best = (pair_t *)malloc(sizeof(pair_t) * (size_t)topk);
  if (best == NULL) {
    fprintf(stderr, "out of host memory for ground truth\n");
    seekdb_cuvs_free(handle);
    return 3;
  }
  long hit10 = 0, hitk = 0;
  for (long q = 0; q < nq; ++q) {
    const float *query = queries + q * dim;
    long kept = 0;
    for (long i = 0; i < n; ++i) {
      const float *rowp = base + i * dim;
      float sum = 0.0f;
      for (long d = 0; d < dim; ++d) {
        const float diff = query[d] - rowp[d];
        sum += diff * diff;
      }
      keep_best(best, &kept, topk, sum, (int32_t)i);
    }
    const int32_t *found = got + q * topk;
    for (long r = 0; r < k10; ++r) {
      for (long c = 0; c < k10; ++c) {
        if (found[r] == best[c].index) { ++hit10; break; }
      }
    }
    for (long r = 0; r < topk; ++r) {
      for (long c = 0; c < topk; ++c) {
        if (found[r] == best[c].index) { ++hitk; break; }
      }
    }
  }
  free(best);
  seekdb_cuvs_free(handle);

  const char *niter = getenv("OB_CAGRA_NN_ITER");
  const char *gd = getenv("OB_CAGRA_GRAPH_DEGREE");
  const char *igd = getenv("OB_CAGRA_INTERMEDIATE_DEGREE");
  printf("n=%ld dim=%ld nq=%ld topk=%ld gd=%s igd=%s niter=%s "
         "build_s=%.3f search_s=%.3f recall@%ld=%.4f recall@%ld=%.4f\n",
         n, dim, nq, topk,
         gd ? gd : "128(default)", igd ? igd : "256(default)", niter ? niter : "50(default)",
         build_seconds, search_seconds,
         k10, (double)hit10 / (double)(nq * k10),
         topk, (double)hitk / (double)(nq * topk));
  fflush(stdout);

  free(base);
  free(queries);
  free(got);
  return 0;
}
