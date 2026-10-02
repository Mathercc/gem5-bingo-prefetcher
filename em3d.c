// em3d.c - electromagnetic wave propagation kernel, as configured in the
// Bingo paper (Table II): 400K nodes, degree 2, span 5, 15% remote.
//
// Written from the description of Olden em3d (Carlisle, 1994) and its
// Split-C ancestor: two node sets, E and H, each a linked list of
// individually allocated nodes. Every node reads the values of `degree`
// nodes of the other set through pointers and subtracts coeff * value.
// A neighbour is "local" (index within +-span of the node's own index)
// with probability 1 - remote%, otherwise anywhere in the other set.
// One iteration updates all E nodes, then all H nodes.
//
// Build: gcc -O2 -std=c11 -static -o em3d em3d.c
// Usage: ./em3d [nodes] [degree] [span] [remote%] [iters] [seed]
//        defaults: 400000 2 5 15 100 1   (nodes = E + H)

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct node {
    double *value;
    struct node *next;
    double **from_values;   // values of the other set this node reads
    double *coeffs;
    int from_count;
} node_t;

static uint64_t rng = 1;

static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static double rand_unit(void)
{
    return (double)(next_rand() >> 11) * (1.0 / 9007199254740992.0);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        perror("malloc");
        exit(1);
    }
    return p;
}

// Allocate one node set: a value array plus one heap node per entry,
// chained in index order.
static node_t **make_set(int n)
{
    node_t **table = xmalloc((size_t)n * sizeof(node_t *));
    double *values = xmalloc((size_t)n * sizeof(double));
    for (int i = 0; i < n; i++) {
        node_t *nd = xmalloc(sizeof(node_t));
        values[i] = rand_unit();
        nd->value = &values[i];
        nd->next = NULL;
        nd->from_count = 0;
        if (i > 0)
            table[i - 1]->next = nd;
        table[i] = nd;
    }
    return table;
}

static int pick_neighbour(int self, int n, int span, int remote_pct)
{
    if ((int)(next_rand() % 100) < remote_pct)
        return (int)(next_rand() % (uint64_t)n);
    int j = self + (int)(next_rand() % (uint64_t)(2 * span + 1)) - span;
    if (j < 0)
        j += n;
    if (j >= n)
        j -= n;
    return j;
}

// Give every node of `dst` `degree` distinct neighbours in `src`.
static void connect(node_t **dst, node_t **src, int n, int degree, int span,
                    int remote_pct)
{
    for (int i = 0; i < n; i++) {
        node_t *nd = dst[i];
        nd->from_values = xmalloc((size_t)degree * sizeof(double *));
        nd->coeffs = xmalloc((size_t)degree * sizeof(double));
        nd->from_count = degree;
        int picked[64];
        for (int d = 0; d < degree; d++) {
            int j, dup;
            do {
                j = pick_neighbour(i, n, span, remote_pct);
                dup = 0;
                for (int k = 0; k < d; k++)
                    dup |= picked[k] == j;
            } while (dup);
            picked[d] = j;
            nd->from_values[d] = src[j]->value;
            nd->coeffs[d] = rand_unit();
        }
    }
}

static void compute_nodes(node_t *list)
{
    for (node_t *nd = list; nd; nd = nd->next) {
        double v = *nd->value;
        for (int i = 0; i < nd->from_count; i++)
            v -= nd->coeffs[i] * *nd->from_values[i];
        *nd->value = v;
    }
}

int main(int argc, char **argv)
{
    int nodes = argc > 1 ? atoi(argv[1]) : 400000;
    int degree = argc > 2 ? atoi(argv[2]) : 2;
    int span = argc > 3 ? atoi(argv[3]) : 5;
    int remote = argc > 4 ? atoi(argv[4]) : 15;
    int iters = argc > 5 ? atoi(argv[5]) : 100;
    rng = argc > 6 ? strtoull(argv[6], NULL, 0) : 1;
    if (nodes < 4 || degree < 1 || degree > 64 || span < 1 ||
        2 * span + 1 < degree || remote < 0 || remote > 100 || iters < 1) {
        fprintf(stderr, "bad arguments\n");
        return 2;
    }

    int n = nodes / 2;   // nodes per set
    node_t **e = make_set(n);
    node_t **h = make_set(n);
    connect(e, h, n, degree, span, remote);
    connect(h, e, n, degree, span, remote);

    for (int it = 0; it < iters; it++) {
        compute_nodes(e[0]);
        compute_nodes(h[0]);
    }

    double sum = 0;
    for (int i = 0; i < n; i++)
        sum += *e[i]->value + *h[i]->value;
    printf("em3d: nodes=%d degree=%d span=%d remote=%d%% iters=%d sum=%.6e\n",
           nodes, degree, span, remote, iters, sum);
    return 0;
}
