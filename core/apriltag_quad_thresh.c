/* (C) 2013-2015, The Regents of The University of Michigan
All rights reserved.

This software may be available under alternative licensing
terms. Contact Edwin Olson, ebolson@umich.edu, for more information.

   Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The views and conclusions contained in the software and documentation are those
of the authors and should not be interpreted as representing official policies,
either expressed or implied, of the FreeBSD Project.
 */

// limitation: image size must be <32768 in width and height. This is
// because we use a fixed-point 16 bit integer representation with one
// fractional bit.
#define _GNU_SOURCE // for M_PI
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "assert_with_unused.h"
#include "apriltag.h"
#include "zarray.h"
#include "zhash.h"
#include "unionfind.h"
#include "timeprofile.h"
#include "zmaxheap.h"
#include "postscript_utils.h"

/*
static inline uint32_t u64hash_1(uint64_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x);
    return (uint32_t) x;
}
*/

static inline double terrible_atan2_quadrant0(double y, double x)
{
    if (x > y) {
        return y/x;
    }
    return 2-x/y;
}

static inline double terrible_atan2(double y, double x)
{
    const double K = 2*M_PI / 8;

    if (x >= 0) {
        if (y >= 0) {
            return K*terrible_atan2_quadrant0(y, x);  // quadrant 1
        } else {
            return -K*terrible_atan2_quadrant0(-y, x); // quadrant 4
        }
    } else {
        if (y >= 0) {
            return K*(4 - terrible_atan2_quadrant0(y, -x)); // quadrant 2
        } else {
            return K*(-4 + terrible_atan2_quadrant0(-y, -x)); // quadrant 3
        }
    }
}

static inline uint32_t u64hash_2(uint64_t x) {
    return (2654435761 * x) >> 32;
    return (uint32_t) x;
}

#define TNAME uint64_zarray_hash
#define TKEYTYPE uint64_t
#define TVALTYPE zarray_t*
#define TKEYHASH(pk) (u64hash_2(*(pk)))
#define TKEYEQUAL(pka, pkb) (*(pka) == *(pkb))
#include "common/thash_impl.h"
#undef TKEYEQUAL
#undef TKEYHASH
#undef TKEYTYPE
#undef TVALTYPE
#undef TNAME

#ifndef M_PI
# define M_PI 3.141592653589793238462643383279502884196
#endif

struct pt
{
    // Note: these represent 2*actual value.
    uint16_t x, y;
    float theta;
};

struct unionfind_task
{
    int y0, y1;
    int w, h, s;
    unionfind_t *uf;
    image_u8_t *edgeim;
};

struct quad_task
{
    zarray_t *clusters;
    int cidx0, cidx1; // [cidx0, cidx1)
    zarray_t *quads;
    apriltag_detector_t *td;
    int w, h;

    image_u8_t *im;
};

struct remove_vertex
{
    size_t i;           // which vertex to remove?
    int left, right; // left vertex, right vertex

    double err;
};

struct segment
{
    int is_vertex;

    // always greater than zero, but right can be > size, which denotes
    // a wrap around back to the beginning of the points. and left < right.
    int left, right;
};

struct line_fit_pt
{
    double Mx, My;
    double Mxx, Myy, Mxy;
    double W; // total weight
};

static inline double sq(double v)
{
    return v*v;
}

static inline int imin(int a, int b)
{
    return a < b ? a : b;
}

static inline int imax(int a, int b)
{
    return a < b ? b : a;
}

static inline void ptsort(struct pt *pts, int sz)
{
#define MAYBE_SWAP(arr,apos,bpos) \
    if (arr[apos].theta > arr[bpos].theta) {                        \
        tmp = arr[apos]; arr[apos] = arr[bpos]; arr[bpos] = tmp;    \
    };

    if (sz <= 1)
        return;

    if (sz == 2) {
        struct pt tmp;
        MAYBE_SWAP(pts, 0, 1);
        return;
    }

    // NB: Using less-branch-intensive sorting networks here on the
    // hunch that it's better for performance.
    if (sz == 3) { // 3 element bubble sort is optimal
        struct pt tmp;
        MAYBE_SWAP(pts, 0, 1);
        MAYBE_SWAP(pts, 1, 2);
        MAYBE_SWAP(pts, 0, 1);
        return;
    }

    if (sz == 4) { // 4 element optimal sorting network.
        struct pt tmp;
        MAYBE_SWAP(pts, 0, 1); // sort each half, like a merge sort
        MAYBE_SWAP(pts, 2, 3);
        MAYBE_SWAP(pts, 0, 2); // minimum value is now at 0.
        MAYBE_SWAP(pts, 1, 3); // maximum value is now at end.
        MAYBE_SWAP(pts, 1, 2); // that only leaves the middle two.
        return;
    }

    if (sz == 5) {
        // this 9-step swap is optimal for a sorting network, but two
        // steps slower than a generic sort.
        struct pt tmp;
        MAYBE_SWAP(pts, 0, 1); // sort each half (3+2), like a merge sort
        MAYBE_SWAP(pts, 3, 4);
        MAYBE_SWAP(pts, 1, 2);
        MAYBE_SWAP(pts, 0, 1);
        MAYBE_SWAP(pts, 0, 3); // minimum element now at 0
        MAYBE_SWAP(pts, 2, 4); // maximum element now at end
        MAYBE_SWAP(pts, 1, 2); // now resort the three elements 1-3.
        MAYBE_SWAP(pts, 2, 3);
        MAYBE_SWAP(pts, 1, 2);
        return;
    }

#undef MAYBE_SWAP

    // a merge sort with temp storage.

    // Use stack storage if it's not too big.
    int stacksz = sz;
    if (stacksz > 1024) {
        stacksz = 0;
    }

    struct pt _tmp_stack[stacksz];
    struct pt *tmp = _tmp_stack;

    if (stacksz == 0) {
        // it was too big, malloc it instead.
        tmp = malloc(sizeof(struct pt) * sz);
    }

    memcpy(tmp, pts, sizeof(struct pt) * sz);

    int asz = sz/2;
    int bsz = sz - asz;

    struct pt *as = &tmp[0];
    struct pt *bs = &tmp[asz];

    ptsort(as, asz);
    ptsort(bs, bsz);

#define MERGE(apos,bpos)                        \
    if (as[apos].theta < bs[bpos].theta)        \
        pts[outpos++] = as[apos++];             \
    else                                        \
        pts[outpos++] = bs[bpos++];

    int apos = 0, bpos = 0, outpos = 0;
    while (apos + 8 < asz && bpos + 8 < bsz) {
        MERGE(apos,bpos); MERGE(apos,bpos); MERGE(apos,bpos); MERGE(apos,bpos);
        MERGE(apos,bpos); MERGE(apos,bpos); MERGE(apos,bpos); MERGE(apos,bpos);
    }

    while (apos < asz && bpos < bsz) {
        MERGE(apos,bpos);
    }

    if (apos < asz) {
        memcpy(&pts[outpos], &as[apos], (asz-apos)*sizeof(struct pt));
    }
    if (bpos < bsz) {
        memcpy(&pts[outpos], &bs[bpos], (bsz-bpos)*sizeof(struct pt));
    }

    if (stacksz == 0) {
        free(tmp);
    }

#undef MERGE
}

// lfps contains *cumulative* moments for N points, with
// index j reflecting points [0,j] (inclusive).
//
// fit a line to the points [i0, i1] (inclusive). i0, i1 are both [0,
// sz) if i1 < i0, we treat this as a wrap around.
void fit_line(struct line_fit_pt *lfps, int sz, int i0, int i1, double *lineparm, double *err, double *mse)
{
    assert(i0 != i1);
    assert(i0 >= 0 && i1 >= 0 && i0 < sz && i1 < sz);

    double Mx, My, Mxx, Myy, Mxy, W;
    int N; // how many points are included in the set?

    if (i0 < i1) {
        N = i1 - i0 + 1;

        Mx  = lfps[i1].Mx;
        My  = lfps[i1].My;
        Mxx = lfps[i1].Mxx;
        Mxy = lfps[i1].Mxy;
        Myy = lfps[i1].Myy;
        W   = lfps[i1].W;

        if (i0 > 0) {
            Mx  -= lfps[i0-1].Mx;
            My  -= lfps[i0-1].My;
            Mxx -= lfps[i0-1].Mxx;
            Mxy -= lfps[i0-1].Mxy;
            Myy -= lfps[i0-1].Myy;
            W   -= lfps[i0-1].W;
        }

    } else {
        // i0 > i1, e.g. [15, 2]. Wrap around.
        assert(i0 > 0);

        Mx  = lfps[sz-1].Mx   - lfps[i0-1].Mx;
        My  = lfps[sz-1].My   - lfps[i0-1].My;
        Mxx = lfps[sz-1].Mxx  - lfps[i0-1].Mxx;
        Mxy = lfps[sz-1].Mxy  - lfps[i0-1].Mxy;
        Myy = lfps[sz-1].Myy  - lfps[i0-1].Myy;
        W   = lfps[sz-1].W    - lfps[i0-1].W;

        Mx  += lfps[i1].Mx;
        My  += lfps[i1].My;
        Mxx += lfps[i1].Mxx;
        Mxy += lfps[i1].Mxy;
        Myy += lfps[i1].Myy;
        W   += lfps[i1].W;

        N = sz - i0 + i1 + 1;
    }

    assert(N >= 2);

    double Ex = Mx / W;
    double Ey = My / W;
    double Cxx = Mxx / W - Ex*Ex;
    double Cxy = Mxy / W - Ex*Ey;
    double Cyy = Myy / W - Ey*Ey;

    double nx, ny;

    if (1) {
        // on iOS about 5% of total CPU spent in these trig functions.
        // 85 ms per frame on 5S, example.pnm
        //
        // XXX this was using the double-precision atan2. Was there a case where
        // we needed that precision? Seems doubtful.
        double normal_theta = .5 * atan2f(-2*Cxy, (Cyy - Cxx));
        nx = cosf(normal_theta);
        ny = sinf(normal_theta);
    } else {
        // 73.5 ms per frame on 5S, example.pnm
        double ty = -2*Cxy;
        double tx = (Cyy - Cxx);
        double mag = ty*ty + tx*tx;

        if (mag == 0) {
            nx = 1;
            ny = 0;
        } else {
            double norm = sqrtf(ty*ty + tx*tx);
            tx /= norm;

            // ty is now sin(2theta)
            // tx is now cos(2theta). We want sin(theta) and cos(theta)

            // due to precision err, tx could still have slightly too large magnitude.
            if (tx > 1) {
                ny = 0;
                nx = 1;
            } else if (tx < -1) {
                ny = 1;
                nx = 0;
            } else {
                // half angle formula
                ny = sqrtf((1 - tx)/2);
                nx = sqrtf((1 + tx)/2);

                // pick a consistent branch cut
                if (ty < 0) {
                    ny = - ny;
                }
            }
        }
    }

    if (lineparm) {
        lineparm[0] = Ex;
        lineparm[1] = Ey;
        lineparm[2] = nx;
        lineparm[3] = ny;
    }

    // sum of squared errors =
    //
    // SUM_i ((p_x - ux)*nx + (p_y - uy)*ny)^2
    // SUM_i  nx*nx*(p_x - ux)^2 + 2nx*ny(p_x -ux)(p_y-uy) + ny*ny*(p_y-uy)*(p_y-uy)
    //  nx*nx*SUM_i((p_x -ux)^2) + 2nx*ny*SUM_i((p_x-ux)(p_y-uy)) + ny*ny*SUM_i((p_y-uy)^2)
    //
    //  nx*nx*N*Cxx + 2nx*ny*N*Cxy + ny*ny*N*Cyy

    // sum of squared errors
    if (err)
        *err = nx*nx*N*Cxx + 2*nx*ny*N*Cxy + ny*ny*N*Cyy;

    // mean squared error
    if (mse)
        *mse = nx*nx*Cxx + 2*nx*ny*Cxy + ny*ny*Cyy;
}

int pt_compare_theta(const void *_a, const void *_b)
{
    struct pt *a = (struct pt*) _a;
    struct pt *b = (struct pt*) _b;

    return (a->theta < b->theta) ? -1 : 1;
}

int err_compare_descending(const void *_a, const void *_b)
{
    const double *a =  _a;
    const double *b =  _b;

    return ((*a) < (*b)) ? 1 : -1;
}

/*

  1. Identify A) white points near a black point and B) black points near a white point.

  2. Find the connected components within each of the classes above,
  yielding clusters of "white-near-black" and
  "black-near-white". (These two classes are kept separate). Each
  segment has a unique id.

  3. For every pair of "white-near-black" and "black-near-white"
  clusters, find the set of points that are in one and adjacent to the
  other. In other words, a "boundary" layer between the two
  clusters. (This is actually performed by iterating over the pixels,
  rather than pairs of clusters.) Critically, this helps keep nearby
  edges from becoming connected.
*/
int quad_segment_maxima(apriltag_detector_t *td, zarray_t *cluster, struct line_fit_pt *lfps, int indices[4])
{
    int sz = zarray_size(cluster);

    // ksz: when fitting points, how many points on either side do we consider?
    // (actual "kernel" width is 2ksz).
    //
    // This value should be about: 0.5 * (points along shortest edge).
    //
    // If all edges were equally-sized, that would give a value of
    // sz/8. We make it somewhat smaller to account for tags at high
    // aspects.

    // XXX Tunable
    int ksz = imin(20, sz / 12);

    // can't fit a quad if there are too few points.
    if (ksz < 2)
        return 0;

//    printf("sz %5d, ksz %3d\n", sz, ksz);

    double errs[sz];

    for (int i = 0; i < sz; i++) {
        fit_line(lfps, sz, (i + sz - ksz) % sz, (i + ksz) % sz, NULL, &errs[i], NULL);
    }

    // apply a low-pass filter to errs
    if (1) {
        double y[sz];

        // how much filter to apply?

        // XXX Tunable
        double sigma = 1; // was 3

        // cutoff = exp(-j*j/(2*sigma*sigma));
        // log(cutoff) = -j*j / (2*sigma*sigma)
        // log(cutoff)*2*sigma*sigma = -j*j;

        // how big a filter should we use? We make our kernel big
        // enough such that we represent any values larger than
        // 'cutoff'.

        // XXX Tunable (though not super useful to change)
        double cutoff = 0.05;
        int fsz = sqrt(-log(cutoff)*2*sigma*sigma) + 1;
        fsz = 2*fsz + 1;

        // For default values of cutoff = 0.05, sigma = 3,
        // we have fsz = 17.
        float f[fsz];

        for (int i = 0; i < fsz; i++) {
            int j = i - fsz / 2;
            f[i] = exp(-j*j/(2*sigma*sigma));
        }

        for (int iy = 0; iy < sz; iy++) {
            double acc = 0;

            for (int i = 0; i < fsz; i++) {
                acc += errs[(iy + i - fsz / 2 + sz) % sz] * f[i];
            }
            y[iy] = acc;
        }

        memcpy(errs, y, sizeof(y));
    }

    int maxima[sz];
    double maxima_errs[sz];
    int nmaxima = 0;

    for (int i = 0; i < sz; i++) {
        if (errs[i] > errs[(i+1)%sz] && errs[i] > errs[(i+sz-1)%sz]) {
            maxima[nmaxima] = i;
            maxima_errs[nmaxima] = errs[i];
            nmaxima++;
        }
    }

    // if we didn't get at least 4 maxima, we can't fit a quad.
    if (nmaxima < 4) {
        return 0;
    }

    // select only the best maxima if we have too many
    int max_nmaxima = td->qtp.max_nmaxima;

    if (nmaxima > max_nmaxima) {
        double maxima_errs_copy[nmaxima];
        memcpy(maxima_errs_copy, maxima_errs, sizeof(maxima_errs_copy));

        // throw out all but the best handful of maxima. Sorts descending.
        qsort(maxima_errs_copy, nmaxima, sizeof(double), err_compare_descending);

        double maxima_thresh = maxima_errs_copy[max_nmaxima];
        int out = 0;
        for (int in = 0; in < nmaxima; in++) {
            if (maxima_errs[in] <= maxima_thresh) {
                continue;
            }
            maxima[out++] = maxima[in];
        }
        nmaxima = out;
    }

    int best_indices[4];
    double best_error = HUGE_VALF;

    double err01, err12, err23, err30;
    double mse01, mse12, mse23, mse30;
    double params01[4], params12[4], params23[4], params30[4];

    // disallow quads where the angle is less than a critical value.
    double max_dot = cos(td->qtp.critical_rad); //25*M_PI/180);

    for (int m0 = 0; m0 < nmaxima - 3; m0++) {
        int i0 = maxima[m0];

        for (int m1 = m0+1; m1 < nmaxima - 2; m1++) {
            int i1 = maxima[m1];

            fit_line(lfps, sz, i0, i1, params01, &err01, &mse01);

            if (mse01 > td->qtp.max_line_fit_mse) {
                continue;
            }

            for (int m2 = m1+1; m2 < nmaxima - 1; m2++) {
                int i2 = maxima[m2];

                fit_line(lfps, sz, i1, i2, params12, &err12, &mse12);
                if (mse12 > td->qtp.max_line_fit_mse) {
                    continue;
                }

                double dot = params01[2]*params12[2] + params01[3]*params12[3];
                if (fabs(dot) > max_dot) {
                    continue;
                }

                for (int m3 = m2+1; m3 < nmaxima; m3++) {
                    int i3 = maxima[m3];

                    fit_line(lfps, sz, i2, i3, params23, &err23, &mse23);
                    if (mse23 > td->qtp.max_line_fit_mse) {
                        continue;
                    }

                    fit_line(lfps, sz, i3, i0, params30, &err30, &mse30);
                    if (mse30 > td->qtp.max_line_fit_mse) {
                        continue;
                    }

                    double err = err01 + err12 + err23 + err30;
                    if (err < best_error) {
                        best_error = err;
                        best_indices[0] = i0;
                        best_indices[1] = i1;
                        best_indices[2] = i2;
                        best_indices[3] = i3;
                    }
                }
            }
        }
    }

    if (best_error == HUGE_VALF) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        indices[i] = best_indices[i];
    }

    if (best_error / sz < td->qtp.max_line_fit_mse) {
        return 1;
    }
    return 0;
}

// returns 0 if the cluster looks bad.
int quad_segment_agg(apriltag_detector_t *td, zarray_t *cluster, struct line_fit_pt *lfps, int indices[4])
{
    int sz = zarray_size(cluster);

    zmaxheap_t *heap = zmaxheap_create(sizeof(struct remove_vertex*));

    // We will initially allocate sz rvs. We then have two types of
    // iterations: some iterations that are no-ops in terms of
    // allocations, and those that remove a vertex and allocate two
    // more children.  This will happen at most (sz-4) times.  Thus we
    // need: sz + 2*(sz-4) entries.

    int rvalloc_pos = 0;
    int rvalloc_size = 3*sz;
    struct remove_vertex *rvalloc = calloc(rvalloc_size, sizeof(struct remove_vertex));

    struct segment *segs = calloc(sz, sizeof(struct segment));

    // populate with initial entries
    for (int i = 0; i < sz; i++) {
        struct remove_vertex *rv = &rvalloc[rvalloc_pos++];
        rv->i = i;
        if (i == 0) {
            rv->left = sz-1;
            rv->right = 1;
        } else {
            rv->left  = i-1;
            rv->right = (i+1) % sz;
        }

        fit_line(lfps, sz, rv->left, rv->right, NULL, NULL, &rv->err);

        zmaxheap_add(heap, &rv, -rv->err);

        segs[i].left = rv->left;
        segs[i].right = rv->right;
        segs[i].is_vertex = 1;
    }

    int nvertices = sz;

    while (nvertices > 4) {
        assert(rvalloc_pos < rvalloc_size);

        struct remove_vertex *rv;
        float err;

        int res = zmaxheap_remove_max(heap, &rv, &err);
        if (!res) {
            return 0;
        }
        assert(res);

        // is this remove_vertex valid? (Or has one of the left/right
        // vertices changes since we last looked?)
        if (!segs[rv->i].is_vertex ||
            !segs[rv->left].is_vertex ||
            !segs[rv->right].is_vertex) {
            continue;
        }

        // we now merge.
        assert(segs[rv->i].is_vertex);

        segs[rv->i].is_vertex = 0;
        segs[rv->left].right = rv->right;
        segs[rv->right].left = rv->left;

        // create the join to the left
        if (1) {
            struct remove_vertex *child = &rvalloc[rvalloc_pos++];
            child->i = rv->left;
            child->left = segs[rv->left].left;
            child->right = rv->right;

            fit_line(lfps, sz, child->left, child->right, NULL, NULL, &child->err);

            zmaxheap_add(heap, &child, -child->err);
        }

        // create the join to the right
        if (1) {
            struct remove_vertex *child = &rvalloc[rvalloc_pos++];
            child->i = rv->right;
            child->left = rv->left;
            child->right = segs[rv->right].right;

            fit_line(lfps, sz, child->left, child->right, NULL, NULL, &child->err);

            zmaxheap_add(heap, &child, -child->err);
        }

        // we now have one less vertex
        nvertices--;
    }

    free(rvalloc);

    int idx = 0;
    for (int i = 0; i < sz; i++) {
        if (segs[i].is_vertex) {
            indices[idx++] = i;
        }
    }

    free(segs);

    return 1;
}

// return 1 if the quad looks okay, 0 if it should be discarded
int fit_quad(apriltag_detector_t *td, image_u8_t *im, zarray_t *cluster, struct quad *quad)
{
    int res = 0;

    int sz = zarray_size(cluster);
    if (sz < 4) { // can't fit a quad to less than 4 points
        return 0;
    }

    /////////////////////////////////////////////////////////////
    // Step 1. Sort points so they wrap around the center of the
    // quad. We will constrain our quad fit to simply partition this
    // ordered set into 4 groups.

    // compute a bounding box so that we can order the points
    // according to their angle WRT the center.
    int32_t xmax = 0, xmin = INT32_MAX, ymax = 0, ymin = INT32_MAX;

    for (size_t pidx = 0; pidx < zarray_size(cluster); pidx++) {
        struct pt *p;
        zarray_get_volatile(cluster, pidx, &p);

        xmax = imax(xmax, p->x);
        xmin = imin(xmin, p->x);

        ymax = imax(ymax, p->y);
        ymin = imin(ymin, p->y);
    }

    // add some noise to (cx,cy) so that pixels get a more diverse set
    // of theta estimates. This will help us remove more points.
    // (Only helps a small amount. The actual noise values here don't
    // matter much at all, but we want them [-1, 1]. (XXX with
    // fixed-point, should range be bigger?)
    double cx = (xmin + xmax) * 0.5 + 0.05118;
    double cy = (ymin + ymax) * 0.5 + -0.028581; // NOTE: I don't understand the intention here -MZ

    for (size_t pidx = 0; pidx < zarray_size(cluster); pidx++) {
        struct pt *p;
        zarray_get_volatile(cluster, pidx, &p);

        double dx = p->x - cx;
        double dy = p->y - cy;

        p->theta = atan2f(dy, dx);
        UNUSED(terrible_atan2);
//        p->theta = terrible_atan2(dy, dx);
    }

    // we now sort the points according to theta. This is a prepatory
    // step for segmenting them into four lines.
    if (1) {
//        zarray_sort(cluster, pt_compare_theta);
        ptsort((struct pt*) cluster->data, zarray_size(cluster));

        // remove duplicate points. (A byproduct of our segmentation system.)
        if (1) {
            int outpos = 1;

            struct pt *last;
            zarray_get_volatile(cluster, 0, &last);

            for (int i = 1; i < sz; i++) {

                struct pt *p;
                zarray_get_volatile(cluster, i, &p);

                if (p->x != last->x || p->y != last->y) {

                    if (i != outpos)  {
                        struct pt *out;
                        zarray_get_volatile(cluster, outpos, &out);
                        memcpy(out, p, sizeof(struct pt));
                    }

                    outpos++;
                }

                last = p;
            }

            cluster->size = outpos;
            sz = outpos;
        }

   } else {
        // This is a counting sort in which we retain at most one
        // point for every bucket; the bucket index is computed from
        // theta. Since a good quad completes a complete revolution,
        // there's reason to think that we should get a good
        // distribution of thetas.  We might "lose" a few points due
        // to collisions, but this shouldn't affect quality very much.

        // XXX tunable. Increase to reduce the likelihood of "losing"
        // points due to collisions.
        int nbuckets = 4*sz;

        #define ASSOC 2
        struct pt v[nbuckets][ASSOC];
        memset(v, 0, sizeof(v));

        // put each point into a bucket.
        for (int i = 0; i < sz; i++) {
            struct pt *p;
            zarray_get_volatile(cluster, i, &p);

            assert(p->theta >= -M_PI && p->theta <= M_PI);

            int bucket = (nbuckets - 1) * (p->theta + M_PI) / (2*M_PI);
            assert(bucket >= 0 && bucket < nbuckets);

            for (int i = 0; i < ASSOC; i++) {
                if (v[bucket][i].theta == 0) {
                    v[bucket][i] = *p;
                    break;
                }
            }
        }

        // collect the points from the buckets and put them back into the array.
        int outsz = 0;
        for (int i = 0; i < nbuckets; i++) {
            for (int j = 0; j < ASSOC; j++) {
                if (v[i][j].theta != 0) {
                    zarray_set(cluster, outsz, &v[i][j], NULL);
                    outsz++;
                }
            }
        }

        zarray_truncate(cluster, outsz);
        sz = outsz;
    }

    if (sz < 4) {
        return 0;
    }

    /////////////////////////////////////////////////////////////
    // Step 2. Precompute statistics that allow line fit queries to be
    // efficiently computed for any contiguous range of indices.

    struct line_fit_pt *lfps = calloc(sz, sizeof(struct line_fit_pt));

    for (int i = 0; i < sz; i++) {
        struct pt *p;
        zarray_get_volatile(cluster, i, &p);

        if (i > 0) {
            memcpy(&lfps[i], &lfps[i-1], sizeof(struct line_fit_pt));
        }

        if (0) {
            // we now undo our fixed-point arithmetic.
            double delta = 0.5;
            double x = p->x * .5 + delta;
            double y = p->y * .5 + delta;
            double W;

            for (int dy = -1; dy <= 1; dy++) {
                int iy = y + dy;

                if (iy < 0 || iy + 1 >= im->height) {
                    continue;
                }

                for (int dx = -1; dx <= 1; dx++) {
                    int ix = x + dx;

                    if (ix < 0 || ix + 1 >= im->width) {
                        continue;
                    }

                    int grad_x = im->buf[iy * im->stride + ix + 1] -
                        im->buf[iy * im->stride + ix - 1];

                    int grad_y = im->buf[(iy+1) * im->stride + ix] -
                        im->buf[(iy-1) * im->stride + ix];

                    W = sqrtf(grad_x*grad_x + grad_y*grad_y) + 1;

//                    double fx = x + dx, fy = y + dy;
                    double fx = ix + .5, fy = iy + .5;
                    lfps[i].Mx  += W * fx;
                    lfps[i].My  += W * fy;
                    lfps[i].Mxx += W * fx * fx;
                    lfps[i].Mxy += W * fx * fy;
                    lfps[i].Myy += W * fy * fy;
                    lfps[i].W   += W;
                }
            }
        } else {
            // we now undo our fixed-point arithmetic.
            double delta = 0.5; // adjust for pixel center bias
            double x = p->x * .5 + delta;
            double y = p->y * .5 + delta;
            int ix = x, iy = y;
            double W = 1;

            if (ix > 0 && ix+1 < im->width && iy > 0 && iy+1 < im->height) {
                int grad_x = im->buf[iy * im->stride + ix + 1] -
                    im->buf[iy * im->stride + ix - 1];

                int grad_y = im->buf[(iy+1) * im->stride + ix] -
                    im->buf[(iy-1) * im->stride + ix];

                // XXX Tunable. How to shape the gradient magnitude?
                W = sqrt(grad_x*grad_x + grad_y*grad_y) + 1;
            }

            double fx = x, fy = y;
            lfps[i].Mx  += W * fx;
            lfps[i].My  += W * fy;
            lfps[i].Mxx += W * fx * fx;
            lfps[i].Mxy += W * fx * fy;
            lfps[i].Myy += W * fy * fy;
            lfps[i].W   += W;
        }
    }

    int indices[4];
    if (1) {
        if (!quad_segment_maxima(td, cluster, lfps, indices)) {
            goto finish;
        }
    } else {
        if (!quad_segment_agg(td, cluster, lfps, indices)) {
            goto finish;
        }
    }

//    printf("%d %d %d %d\n", indices[0], indices[1], indices[2], indices[3]);

    if (0) {
        // no refitting here; just use those points as the vertices.
        // Note, this is useful for debugging, but pretty bad in
        // practice since this code path also omits several
        // plausibility checks that save us tons of time in quad
        // decoding.
        for (int i = 0; i < 4; i++) {
            struct pt *p;
            zarray_get_volatile(cluster, indices[i], &p);

            quad->p[i][0] = .5*p->x; // undo fixed-point arith.
            quad->p[i][1] = .5*p->y;
        }

        res = 1;

    } else {
        double lines[4][4];

        for (int i = 0; i < 4; i++) {
            int i0 = indices[i];
            int i1 = indices[(i+1)&3];

            if (0) {
                // if there are enough points, skip the points near the corners
                // (because those tend not to be very good.)
                if (i1-i0 > 8) {
                    int t = (i1-i0)/6;
                    if (t < 0) {
                        t = -t;
                    }

                    i0 = (i0 + t) % sz;
                    i1 = (i1 + sz - t) % sz;
                }
            }

            double err;
            fit_line(lfps, sz, i0, i1, lines[i], NULL, &err);

            // XXX VALUE?
            if (err > td->qtp.max_line_fit_mse) {
                res = 0;
                goto finish;
            }
        }

        for (int i = 0; i < 4; i++) {
            // solve for the intersection of lines (i) and (i+1)&3.
            // p0 + lambda0*u0 = p1 + lambda1*u1, where u0 and u1
            // are the line directions.
            //
            // lambda0*u0 - lambda1*u1 = (p1 - p0)
            //
            // rearrange (solve for lambdas)
            //
            // [u0_x   -u1_x ] [lambda0] = [ p1_x - p0_x ]
            // [u0_y   -u1_y ] [lambda1]   [ p1_y - p0_y ]
            //
            // remember that lines[i][0,1] = p, lines[i][2,3] = NORMAL vector.
            // We want the unit vector, so we need the perpendiculars. Thus, below
            // we have swapped the x and y components and flipped the y components.

            double A00 =  lines[i][3],  A01 = -lines[(i+1)&3][3];
            double A10 =  -lines[i][2],  A11 = lines[(i+1)&3][2];
            double B0 = -lines[i][0] + lines[(i+1)&3][0];
            double B1 = -lines[i][1] + lines[(i+1)&3][1];

            double det = A00 * A11 - A10 * A01;

            // inverse.
            double W00 = A11 / det, W01 = -A01 / det;
            if (fabs(det) < 0.001) {
                res = 0;
                goto finish;
            }

            // solve
            double L0 = W00*B0 + W01*B1;

            // compute intersection
            quad->p[i][0] = lines[i][0] + L0*A00;
            quad->p[i][1] = lines[i][1] + L0*A10;

            if (0) {
                // we should get the same intersection starting
                // from point p1 and moving L1*u1.
                double W10 = -A10 / det, W11 = A00 / det;
                double L1 = W10*B0 + W11*B1;

                double x = lines[(i+1)&3][0] - L1*A10;
                double y = lines[(i+1)&3][1] - L1*A11;
                
                UNUSED(x); UNUSED(y);
                
                assert(fabs(x - quad->p[i][0]) < 0.001 &&
                       fabs(y - quad->p[i][1]) < 0.001);
            }

            res = 1;
        }
    }

    // reject quads with edges that are too short or long.
    if (1) {
        for (int i = 0; i < 3; i++) {
            double dist2 = sq(quad->p[i][0] - quad->p[i+1][0]) +
                sq(quad->p[i][1] - quad->p[i+1][1]);

            if (dist2 < sq(6) || dist2 > sq(4096)) {
                res = 0;
                goto finish;
            }
        }
    }

    // reject quads whose cumulative angle change isn't equal to 2PI
    if (1) {
        double total = 0;

        for (int i = 0; i < 4; i++) {
            int i0 = i, i1 = (i+1)&3, i2 = (i+2)&3;

            double theta0 = atan2f(quad->p[i0][1] - quad->p[i1][1],
                                   quad->p[i0][0] - quad->p[i1][0]);
            double theta1 = atan2f(quad->p[i2][1] - quad->p[i1][1],
                                   quad->p[i2][0] - quad->p[i1][0]);

            double dtheta = theta0 - theta1;
            if (dtheta < 0) {
                dtheta += 2*M_PI;
            }

            if (dtheta < td->qtp.critical_rad || dtheta > (M_PI - td->qtp.critical_rad)) {
                res = 0;
            }

            total += dtheta;
        }

        if (total < 6.2 || total > 6.4) {
            res = 0;
            goto finish;
        }
    }

    // adjust pixel coordinates; all math up 'til now uses pixel
    // coordinates in which (0,0) is the lower left corner. But each
    // pixel actually spans from to [x, x+1), [y, y+1) the mean value of which
    // is +.5 higher than x & y.
/*    double delta = .5;
    for (int i = 0; i < 4; i++) {
        quad->p[i][0] += delta;
        quad->p[i][1] += delta;
    }
*/
  finish:
/*
    if (res) {
        static image_u8_t *im;

        if (im == NULL)
            im = image_u8_create(1920,1080);

        for (int j = 0; j < 4; j++) {
            int i0 = indices[j];
            int i1 = indices[(j+1)&3];

            if (i1 < i0)
                i1 += sz;

            for (int i = i0; i <= i1; i++) {
                struct pt *p;
                // XXX Needs adjusting for fixed-point
                zarray_get_volatile(cluster, i % sz, &p);
                im->buf[((int) p->y)*im->stride + ((int) p->x)] = 64 + 128*(j%2);
            }
        }

        char name[1024];
        sprintf(name, "debug_seg.pnm", utime_now());
        image_u8_write_pnm(im, name);
    }
*/

    free(lfps);

    return res;
}

#define DO_UNIONFIND(dx, dy) if (edgeim->buf[y*s + dy*s + x + dx] == v) unionfind_connect(uf, y*w + x, y*w + dy*w + x + dx);
static inline void do_unionfind_line(unionfind_t *uf, image_u8_t *edgeim, int h, int w, int s, int y)
{
    assert(y+1 < edgeim->height);

    for (int x = 1; x < w - 1; x++) {
        uint8_t v = edgeim->buf[y*s + x];
        if (v==0) {
            continue;
        }

        // (dx,dy) pairs for 8 connectivity:
        //          (REFERENCE) (1, 0)
        // (-1, 1)    (0, 1)    (1, 1)
        //
        DO_UNIONFIND(1, 0);
        DO_UNIONFIND(-1, 1);
        DO_UNIONFIND(0, 1);
        DO_UNIONFIND(1, 1);
    }
}
#undef DO_UNIONFIND

static void do_unionfind_task(void *p)
{
    struct unionfind_task *task = (struct unionfind_task*) p;

    for (int y = task->y0; y < task->y1; y++) {
        do_unionfind_line(task->uf, task->edgeim, task->h, task->w, task->s, y);
    }
}

static void do_quad_task(void *p)
{
    struct quad_task *task = (struct quad_task*) p;

    zarray_t *clusters = task->clusters;
    zarray_t *quads = task->quads;
    apriltag_detector_t *td = task->td;
    int w = task->w, h = task->h;

    for (int cidx = task->cidx0; cidx < task->cidx1; cidx++) {

        zarray_t *cluster;
        zarray_get(clusters, cidx, &cluster);

        if (zarray_size(cluster) < (unsigned int)td->qtp.min_cluster_pixels) {
            continue;
        }

        // a cluster should contain only boundary points around the
        // tag. it cannot be bigger than the whole screen. (Reject
        // large connected blobs that will be prohibitively slow to
        // fit quads to.)
        if (zarray_size(cluster) > (unsigned int)4*(w+h)) {
            continue;
        }

        struct quad quad;
        memset(&quad, 0, sizeof(struct quad));

        if (fit_quad(td, task->im, cluster, &quad)) {
            pthread_mutex_lock(&td->mutex);

            zarray_add(quads, &quad);
            pthread_mutex_unlock(&td->mutex);
        }
    }
}

image_u8_t *threshold(apriltag_detector_t *td, image_u8_t *im)
{
    int w = im->width, h = im->height, s = im->stride;
    assert(w < 32768);
    assert(h < 32768);

    image_u8_t *threshim = image_u8_create(w, h);
    assert(threshim->stride == s);

    // The idea is to find the maximum and minimum values in a
    // window around each pixel. If it's a contrast-free region
    // (max-min is small), don't try to binarize. Otherwise,
    // threshold according to (max+min)/2.

    // however, computing max/min around every pixel is needlessly
    // expensive. We compute max/min for tiles. To avoid artifacts
    // that arise when high-contrast features appear near a tile
    // edge (and thus moving from one tile to another results in a
    // large change in max/min value), the max/min values used for
    // any pixel are computed from all 3x3 surrounding tiles. Thus,
    // the max/min sampling area for nearby pixels overlap by at least
    // on tile.
    //
    // The important thing is that the windows be large enough to
    // capture edge transitions; the tag does not need to fit into
    // a tile.

    // XXX Tunable
    int tilesz = 4;

    int tw = w/tilesz + 1;
    int th = h/tilesz + 1;

    uint8_t *im_max = calloc(tw*th, sizeof(uint8_t));
    uint8_t *im_min = calloc(tw*th, sizeof(uint8_t));

    // first, collect min/max statistics for each tile
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            uint8_t max = 0, min = 255;

            for (int dy = 0; dy < tilesz; dy++) {
                if (ty*tilesz+dy >= h) {
                    continue;
                }

                for (int dx = 0; dx < tilesz; dx++) {
                    if (tx*tilesz+dx >= w) {
                        continue;
                    }

                    uint8_t v = im->buf[(ty*tilesz+dy)*s + tx*tilesz + dx];
                    if (v < min) {
                        min = v;
                    }
                    if (v > max) {
                        max = v;
                    }
                }
            }

            im_max[ty*tw+tx] = max;
            im_min[ty*tw+tx] = min;
        }
    }

    // second, apply 3x3 max/min convolution to "blur" these values
    // over larger areas. This reduces artifacts due to abrupt changes
    // in the threshold value.
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            uint8_t max = 0, min = 255;

            for (int dy = -1; dy <= 1; dy++) {
                if (ty+dy < 0 || ty+dy >= th) {
                    continue;
                }
                for (int dx = -1; dx <= 1; dx++) {
                    if (tx+dx < 0 || tx+dx >= tw) {
                        continue;
                    }

                    uint8_t m = im_max[(ty+dy)*tw+tx+dx];
                    if (m > max) {
                        max = m;
                    }
                    m = im_min[(ty+dy)*tw+tx+dx];
                    if (m < min) {
                        min = m;
                    }
                }
            }

            // XXX Tunable
            if (max - min < td->qtp.min_white_black_diff) {
                continue;
            }

            // argument for biasing towards dark; specular highlights
            // can be substantially brighter than white tag parts
            uint8_t thresh = min + (max - min) / 2;

            for (int dy = 0; dy < tilesz; dy++) {
                int y = ty*tilesz + dy;
                if (y >= h) {
                    continue;
                }

                for (int dx = 0; dx < tilesz; dx++) {
                    int x = tx*tilesz + dx;
                    if (x >= w) {
                        continue;
                    }

                    uint8_t v = im->buf[y*s+x];
                    threshim->buf[y*s+x] = v > thresh;
                }
            }
        }
    }

    free(im_min);
    free(im_max);

    timeprofile_stamp(td->tp, "threshold");

    return threshim;
}

// basically the same as threshold(), but assumes the input image is a
// bayer image. It collects statistics separately for each 2x2 block
// of pixels.
image_u8_t *threshold_bayer(apriltag_detector_t *td, image_u8_t *im)
{
    int w = im->width, h = im->height, s = im->stride;

    image_u8_t *threshim = image_u8_create(w, h);
    assert(threshim->stride == s);

    int tilesz = 32;
    assert((tilesz & 1) == 0); // must be multiple of 2

    int tw = w/tilesz + 1;
    int th = h/tilesz + 1;

    uint8_t *im_max[4], *im_min[4];
    for (int i = 0; i < 4; i++) {
        im_max[i] = calloc(tw*th, sizeof(uint8_t));
        im_min[i] = calloc(tw*th, sizeof(uint8_t));
    }

    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {

            uint8_t max[4] = { 0, 0, 0, 0};
            uint8_t min[4] = { 255, 255, 255, 255 };

            for (int dy = 0; dy < tilesz; dy++) {
                if (ty*tilesz+dy >= h) {
                    continue;
                }

                for (int dx = 0; dx < tilesz; dx++) {
                    if (tx*tilesz+dx >= w) {
                        continue;
                    }

                    // which bayer element is this pixel?
                    int idx = (2*(dy&1) + (dx&1));

                    uint8_t v = im->buf[(ty*tilesz+dy)*s + tx*tilesz + dx];
                    if (v < min[idx]) {
                        min[idx] = v;
                    }
                    if (v > max[idx]) {
                        max[idx] = v;
                    }
                }
            }

            for (int i = 0; i < 4; i++) {
                im_max[i][ty*tw+tx] = max[i];
                im_min[i][ty*tw+tx] = min[i];
            }
        }
    }

    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {

            uint8_t max[4] = { 0, 0, 0, 0};
            uint8_t min[4] = { 255, 255, 255, 255 };

            for (int dy = -1; dy <= 1; dy++) {
                if (ty+dy < 0 || ty+dy >= th) {
                    continue;
                }
                for (int dx = -1; dx <= 1; dx++) {
                    if (tx+dx < 0 || tx+dx >= tw) {
                        continue;
                    }

                    for (int i = 0; i < 4; i++) {
                        uint8_t m = im_max[i][(ty+dy)*tw+tx+dx];
                        if (m > max[i]) {
                            max[i] = m;
                        }
                        m = im_min[i][(ty+dy)*tw+tx+dx];
                        if (m < min[i]) {
                            min[i] = m;
                        }
                    }
                }
            }

            // XXX CONSTANT
//            if (max - min < 30)
//                continue;

            // argument for biasing towards dark; specular highlights
            // can be substantially brighter than white tag parts
            uint8_t thresh[4];
            for (int i = 0; i < 4; i++) {
                thresh[i] = min[i] + (max[i] - min[i]) / 2;
            }

            for (int dy = 0; dy < tilesz; dy++) {
                int y = ty*tilesz + dy;
                if (y >= h) {
                    continue;
                }

                for (int dx = 0; dx < tilesz; dx++) {
                    int x = tx*tilesz + dx;
                    if (x >= w) {
                        continue;
                    }

                    // which bayer element is this pixel?
                    int idx = (2*(y&1) + (x&1));

                    uint8_t v = im->buf[y*s+x];
                    threshim->buf[y*s+x] = v > thresh[idx];
                }
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        free(im_min[i]);
        free(im_max[i]);
    }

    timeprofile_stamp(td->tp, "threshold");

    return threshim;
}

zarray_t *apriltag_quad_thresh(apriltag_detector_t *td, image_u8_t *im)
{


    if (td->quad_contours) {
        fprintf(stderr, "quad_contours is set in tag detector!\n");
        assert(!td->quad_contours);
        exit(1);
    }
  
    ////////////////////////////////////////////////////////
    // step 1. threshold the image, creating the edge image.

    int w = im->width, h = im->height, s = im->stride;

    image_u8_t *threshim = threshold(td, im);
    assert(threshim->stride == s);

    image_u8_t *edgeim = image_u8_create(w, h);

    if (1) {
        image_u8_t *sumim = image_u8_create(w, h);

        // apply a horizontal sum kernel of width 3
        for (int y = 0; y < h; y++) {
            for (int x = 1; x+1 < w; x++) {

                sumim->buf[y*s + x] =
                    threshim->buf[y*s + x - 1] +
                    threshim->buf[y*s + x + 0] +
                    threshim->buf[y*s + x + 1];
            }
        }
        timeprofile_stamp(td->tp, "sumim");

        // deglitch
        if (td->qtp.deglitch) {
            for (int y = 1; y+1 < h; y++) {
                for (int x = 1; x+1 < w; x++) {
                    // edge: black pixel next to white pixel
                    if (threshim->buf[y*s + x] == 0 &&
                        sumim->buf[y*s + x - s] + sumim->buf[y*s + x] + sumim->buf[y*s + x + s] == 8) {
                        threshim->buf[y*s + x] = 1;
                        sumim->buf[y*s + x - 1]++;
                        sumim->buf[y*s + x + 0]++;
                        sumim->buf[y*s + x + 1]++;
                    }

                    if (threshim->buf[y*s + x] == 1 &&
                        sumim->buf[y*s + x - s] + sumim->buf[y*s + x] + sumim->buf[y*s + x + s] == 1) {
                        threshim->buf[y*s + x] = 0;
                        sumim->buf[y*s + x - 1]--;
                        sumim->buf[y*s + x + 0]--;
                        sumim->buf[y*s + x + 1]--;
                   }
                }
            }

            timeprofile_stamp(td->tp, "deglitch");
        }

        // apply a vertical sum kernel of width 3; check if any
        // over-threshold pixels are adjacent to an under-threshold
        // pixel.
        //
        // There are two types of edges: white pixels neighboring a
        // black pixel, and black pixels neighboring a white pixel. We
        // label these separately.  (Values 0xc0 and 0x3f are picked
        // such that they add to 255 (see below) and so that they can be
        // viewed as pixel intensities for visualization purposes.)
        //
        // symmetry of detection. We don't want to use JUST "black
        // near white" (or JUST "white near black"), because that
        // biases the detection towards one side of the edge. This
        // measurably reduces detection performance.
        //
        // On large tags, we could treat "neighbor" pixels the same
        // way. But on very small tags, there may be other edges very
        // near the tag edge. Since each of these edges is effectively
        // two pixels thick (the white pixel near the black pixel, and
        // the black pixel near the white pixel), it becomes likely
        // that these two nearby edges will actually touch.
        //
        // A partial solution to this problem is to define edges to be
        // adjacent white-near-black and black-near-white pixels.
        //

        for (int y = 1; y+1 < h; y++) {
            for (int x = 1; x+1 < w; x++) {
                if (threshim->buf[y*s + x] == 0) {

                    // edge: black pixel next to white pixel
                    if (sumim->buf[y*s + x - s] + sumim->buf[y*s + x] + sumim->buf[y*s + x + s] > 0) {
                        edgeim->buf[y*s + x] = 0xc0;
                    }
                } else {
                    // edge: white pixel next to black pixel when both
                    // edge types are on, we get less bias towards one
                    // side of the edge.
                    if (sumim->buf[y*s + x - s] + sumim->buf[y*s + x] + sumim->buf[y*s + x + s] < 9) {
                        edgeim->buf[y*s + x] = 0x3f;
                    }
                }
            }
        }

        if (td->debug) {
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    threshim->buf[y*s + x] *= 255;
                }
            }

            image_u8_write_pnm(threshim, "debug_threshold.pnm");
            image_u8_write_pnm(edgeim, "debug_edge.pnm");
//            image_u8_destroy(edgeim2);
        }

        image_u8_destroy(threshim);
        image_u8_destroy(sumim);
    }

    timeprofile_stamp(td->tp, "edges");

    ////////////////////////////////////////////////////////
    // step 2. find connected components.

    unionfind_t *uf = unionfind_create(w * h);

    if (td->nthreads <= 1) {
        for (int y = 0; y < h - 1; y++) {
            do_unionfind_line(uf, edgeim, h, w, s, y);
        }
    } else {
        int sz = h - 1;
        int chunksize = 1 + sz / (APRILTAG_TASKS_PER_THREAD_TARGET * td->nthreads);
        struct unionfind_task tasks[sz / chunksize + 1];

        int ntasks = 0;

        for (int i = 0; i < sz; i += chunksize) {
            // each task will process [y0, y1). Note that this attaches
            // each cell to the right and down, so row y1 *is* potentially modified.
            //
            // for parallelization, make sure that each task doesn't touch rows
            // used by another thread.
            tasks[ntasks].y0 = i;
            tasks[ntasks].y1 = imin(sz, i + chunksize - 1);
            tasks[ntasks].h = h;
            tasks[ntasks].w = w;
            tasks[ntasks].s = s;
            tasks[ntasks].uf = uf;
            tasks[ntasks].edgeim = edgeim;

            workerpool_add_task(td->wp, do_unionfind_task, &tasks[ntasks]);
            ntasks++;
        }

        workerpool_run(td->wp);

        // XXX stitch together the different chunks.
        for (int i = 0; i + 1 < ntasks; i++) {
            do_unionfind_line(uf, edgeim, h, w, s, tasks[i].y1);
        }
    }

    timeprofile_stamp(td->tp, "unionfind");

    uint64_zarray_hash_t *clustermap = uint64_zarray_hash_create();

    for (int y = 1; y < h-1; y++) {
        for (int x = 1; x < w-1; x++) {

            uint8_t v0 = edgeim->buf[y*s + x];
            if (v0 == 0) {
                continue;
            }

            uint64_t rep0 = unionfind_get_representative(uf, y*w + x);

            // 8 connectivity. (4 neighbors to check).
//            for (int dy = 0; dy <= 1; dy++) {
//                for (int dx = 1-2*dy; dx <= 1; dx++) {

#define DO_CONN(dx, dy)                                                 \
            if (1) {                                                    \
                uint8_t v1 = edgeim->buf[y*s + dy*s + x + dx];          \
                if (v0 + v1 == 255) {                                   \
                    uint64_t rep1 = unionfind_get_representative(uf, y*w + dy*w + x + dx); \
                    uint64_t clusterid;                                 \
                    if (rep0 < rep1)                                    \
                        clusterid = (rep1 << 32) + rep0;                \
                    else                                                \
                        clusterid = (rep0 << 32) + rep1;                \
                    zarray_t *cluster = NULL;                           \
                    if (!uint64_zarray_hash_get(clustermap, &clusterid, &cluster)) { \
                        cluster = zarray_create(sizeof(struct pt));     \
                        uint64_zarray_hash_put(clustermap, &clusterid, &cluster, NULL, NULL); \
                    }                                                   \
                    struct pt p = { .x = 2*x + 2*dx, .y = 2*y + 2*dy }; \
                    zarray_add(cluster, &p);                            \
                    struct pt p2 = { .x = 2*x, .y = 2*y };              \
                    zarray_add(cluster, &p2);                            \
                }                                                       \
            }

            // do 4 connectivity
            DO_CONN(1, 0);
            DO_CONN(0, 1);
#undef DO_CONN

/*
            // 4 connectivity. (2 neighbors to check)
            for (int n = 1; n <= 2; n++) {
                int dy = n & 1;
                int dx = n >> 1;

                uint8_t v1 = edgeim->buf[(y+dy)*s + x + dx];
                if (v0 + v1 != 255)
                    continue;
                uint64_t rep1 = unionfind_get_representative(uf, (y+dy)*w + x+dx);

// No. The segmented regions can be smaller than min_cluster_pixels,
// with the region between them still bigger.
//
//                if (unionfind_get_set_size(uf, rep1) < td->qtp.min_cluster_pixels)
//                    continue;

                uint64_t clusterid;
                if (rep0 < rep1)
                    clusterid = (rep1 << 32) + rep0;
                else
                    clusterid = (rep0 << 32) + rep1;

                zarray_t *cluster = NULL;

                if (!uint64_zarray_hash_get(clustermap, &clusterid, &cluster)) {
                    cluster = zarray_create(sizeof(struct pt));
                    if (uint64_zarray_hash_put(clustermap, &clusterid, &cluster, NULL, NULL))
                        assert(0);
                }

                // Add the point half-way between the two pixels. We
                // use a fixed-point representation with a scale
                // factor of 2. (This scale factor is undone when
                // computing quads.)
                if (1) {
                    struct pt p = { .x = 2*x + dx, .y = 2*y + dy };
                    zarray_add(cluster, &p);
                }
            }
*/
        }
    }

    // make segmentation image.
    if (td->debug) {
        image_u8_t *d = image_u8_create(w, h);
        assert(d->stride == s);

        uint8_t *colors = (uint8_t*) calloc(w*h, 1);

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t v = unionfind_get_representative(uf, y*w+x);

                if (unionfind_get_set_size(uf, v) < (unsigned int)td->qtp.min_cluster_pixels) {
                    continue;
                }

                uint8_t color = colors[v];

                if (color == 0) {
                    const int bias = 20;
                    color = bias + (random() % (255-bias));
                    colors[v] = color;
                }

                float mix = 0.7;
                mix = 1.0;
                d->buf[y*d->stride + x] = mix*color + (1-mix)*im->buf[y*im->stride + x];
            }
        }

        free(colors);

        image_u8_write_pnm(d, "debug_segmentation.pnm");
        image_u8_destroy(d);
    }

    timeprofile_stamp(td->tp, "make clusters");


    ////////////////////////////////////////////////////////
    // step 3. process each connected component.
    zarray_t *clusters = zarray_create(sizeof(zarray_t*)); //, uint64_zarray_hash_size(clustermap));
    if (1) {
        uint64_zarray_hash_iterator_t zit;
        uint64_zarray_hash_iterator_init(clustermap, &zit);
        uint64_t key;
        zarray_t *value;
        while (uint64_zarray_hash_iterator_next(&zit, &key, &value)) {
            zarray_add(clusters, &value);
        }
    }

//    uint64_zarray_hash_performance(clustermap);
    uint64_zarray_hash_destroy(clustermap);

    zarray_t *quads = zarray_create(sizeof(struct quad));

    int sz = zarray_size(clusters);
    int chunksize = 1 + sz / (APRILTAG_TASKS_PER_THREAD_TARGET * td->nthreads);
    struct quad_task tasks[sz / chunksize + 1];

    int ntasks = 0;
    for (int i = 0; i < sz; i += chunksize) {
        tasks[ntasks].td = td;
        tasks[ntasks].cidx0 = i;
        tasks[ntasks].cidx1 = imin(sz, i + chunksize);
        tasks[ntasks].h = h;
        tasks[ntasks].w = w;
        tasks[ntasks].quads = quads;
        tasks[ntasks].clusters = clusters;
        tasks[ntasks].im = im;

        workerpool_add_task(td->wp, do_quad_task, &tasks[ntasks]);
        ntasks++;
    }

    workerpool_run(td->wp);

    timeprofile_stamp(td->tp, "fit quads to clusters");

    if (td->debug) {
        FILE *f = fopen("debug_lines.ps", "w");
        fprintf(f, "%%!PS\n\n");

        image_u8_t *im2 = image_u8_copy(im);
        image_u8_darken(im2);
        image_u8_darken(im2);

        // assume letter, which is 612x792 points.
        double scale = fmin(612.0/im->width, 792.0/im2->height);
        fprintf(f, "%.15f %.15f scale\n", scale, scale);
        fprintf(f, "0 %d translate\n", im2->height);
        fprintf(f, "1 -1 scale\n");

        postscript_image(f, im);

        for (size_t i = 0; i < zarray_size(quads); i++) {
            struct quad *q;
            zarray_get_volatile(quads, i, &q);

            float rgb[3];
            int bias = 100;

            for (size_t i = 0; i < 3; i++) {
                rgb[i] = bias + (random() % (255-bias));
            }

            fprintf(f, "%f %f %f setrgbcolor\n", rgb[0]/255.0f, rgb[1]/255.0f, rgb[2]/255.0f);
            fprintf(f, "%.15f %.15f moveto %.15f %.15f lineto %.15f %.15f lineto %.15f %.15f lineto %.15f %.15f lineto stroke\n",
                    q->p[0][0], q->p[0][1],
                    q->p[1][0], q->p[1][1],
                    q->p[2][0], q->p[2][1],
                    q->p[3][0], q->p[3][1],
                    q->p[0][0], q->p[0][1]);
        }

        fclose(f);
    }

//        printf("  %d %d %d %d\n", indices[0], indices[1], indices[2], indices[3]);

/*
        if (td->debug) {
            for (int i = 0; i < 4; i++) {
            int i0 = indices[i];
                int i1 = indices[(i+1)&3];

                if (i1 < i0)
                    i1 += zarray_size(cluster);

                for (int j = i0; j <= i1; j++) {
                    struct pt *p;
                    zarray_get_volatile(cluster, j % zarray_size(cluster), &p);

                    edgeim->buf[p->y*edgeim->stride + p->x] = 30+64*i;
                }
            }
            } */

    unionfind_destroy(uf);

    for (size_t i = 0; i < zarray_size(clusters); i++) {
        zarray_t *cluster;
        zarray_get(clusters, i, &cluster);
        zarray_destroy(cluster);
    }

    zarray_destroy(clusters);

    image_u8_destroy(edgeim);

    return quads;
}
