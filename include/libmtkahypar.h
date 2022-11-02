/*******************************************************************************
 * MIT License
 *
 * This file is part of Mt-KaHyPar.
 *
 * Copyright (C) 2020 Tobias Heuer <tobias.heuer@kit.edu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/

#ifndef LIBKAHYPAR_H
#define LIBKAHYPAR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef KAHYPAR_API
#  ifdef _WIN32
#     if defined(KAHYPAR_BUILD_SHARED)  /* build dll */
#         define KAHYPAR_API __declspec(dllexport)
#     elif !defined(KAHYPAR_BUILD_STATIC)  /* use dll */
#         define KAHYPAR_API __declspec(dllimport)
#     else  /* static library */
#         define KAHYPAR_API
#     endif
#  else
#     if __GNUC__ >= 4
#         define KAHYPAR_API __attribute__ ((visibility("default")))
#     else
#         define KAHYPAR_API
#     endif
#  endif
#endif

struct mt_kahypar_context_s;
typedef struct mt_kahypar_context_s mt_kahypar_context_t;

typedef unsigned long int mt_kahypar_hypernode_id_t;
typedef unsigned long int mt_kahypar_hyperedge_id_t;
typedef int mt_kahypar_hypernode_weight_t;
typedef int mt_kahypar_hyperedge_weight_t;
typedef unsigned int mt_kahypar_partition_id_t;

KAHYPAR_API mt_kahypar_context_t* mt_kahypar_context_new();
KAHYPAR_API void mt_kahypar_context_free(mt_kahypar_context_t* kahypar_context);
KAHYPAR_API void mt_kahypar_configure_context_from_file(mt_kahypar_context_t* kahypar_context,
                                                        const char* ini_file_name);

KAHYPAR_API void mt_kahypar_initialize_thread_pool(const size_t num_threads,
                                                   const bool interleaved_allocations);

KAHYPAR_API void mt_kahypar_read_hypergraph_from_file(const char* file_name,
                                                      mt_kahypar_hypernode_id_t* num_vertices,
                                                      mt_kahypar_hyperedge_id_t* num_hyperedges,
                                                      size_t** hyperedge_indices,
                                                      mt_kahypar_hyperedge_id_t** hyperedges,
                                                      mt_kahypar_hyperedge_weight_t** hyperedge_weights,
                                                      mt_kahypar_hypernode_weight_t** vertex_weights);

KAHYPAR_API void mt_kahypar_partition(const mt_kahypar_hypernode_id_t num_vertices,
                                      const mt_kahypar_hyperedge_id_t num_hyperedges,
                                      const double epsilon,
                                      const mt_kahypar_partition_id_t num_blocks,
                                      const int seed,
                                      const mt_kahypar_hypernode_weight_t* vertex_weights,
                                      const mt_kahypar_hyperedge_weight_t* hyperedge_weights,
                                      const size_t* hyperedge_indices,
                                      const mt_kahypar_hyperedge_id_t* hyperedges,
                                      mt_kahypar_hyperedge_weight_t* objective,
                                      mt_kahypar_context_t* kahypar_context,
                                      mt_kahypar_partition_id_t* partition,
                                      const bool verbose = false);


#ifdef __cplusplus
}
#endif

#endif    // LIBKAHYPAR_H