/*
 * QuickRank - A C++ suite of Learning to Rank algorithms
 * Webpage: http://quickrank.isti.cnr.it/
 * Contact: quickrank@isti.cnr.it
 *
 * Unless explicitly acquired and licensed from Licensor under another
 * license, the contents of this file are subject to the Reciprocal Public
 * License ("RPL") Version 1.5, or subsequent versions as allowed by the RPL,
 * and You may not copy or use this file in either source code or executable
 * form, except in compliance with the terms and conditions of the RPL.
 *
 * All software distributed under the RPL is provided strictly on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, AND
 * LICENSOR HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT, OR NON-INFRINGEMENT. See the RPL for specific
 * language governing rights and limitations under the RPL.
 *
 * Contributor:
 *   HPC. Laboratory - ISTI - CNR - http://hpc.isti.cnr.it/
 */
#pragma once

#include <cfloat>
#include <cmath>
#include <cstring>

#include "utils/maxheap.h"
#include "data/vertical_dataset.h"
#include "learning/tree/rtnode.h"
#include "learning/tree/rtnode_histogram.h"

class RTNodeEnriched {
 public:
  RTNode* node = nullptr;
  RTNode* parent = nullptr;
  size_t depth;

  RTNodeEnriched(RTNode* node, RTNode* parent, size_t depth) :
      node(node), parent(parent), depth(depth) {};
};

typedef MaxHeap<RTNode *> rt_maxheap;
typedef MaxHeap<RTNodeEnriched *> rt_maxheap_enriched;

class RegressionTree {
 protected:
  // 0 for unlimited number of nodes (the size of the tree will then be
  // controlled only by minls)
  const size_t nrequiredleaves;
  //minls > 0
  const size_t minls;
  quickrank::data::VerticalDataset *training_dataset = NULL;
  double *training_labels = NULL;
  RTNode **leaves = NULL;
  size_t nleaves = 0;
  RTNode *root = NULL;
  // see collapse_leaves_ in mart
  float collapse_leaves_factor;

 public:
  RegressionTree(size_t nrequiredleaves, quickrank::data::VerticalDataset *dps,
                 double *labels, size_t minls, float collapse_leaves_factor)
      : nrequiredleaves(nrequiredleaves),
        minls(minls),
        training_dataset(dps),
        training_labels(labels),
        collapse_leaves_factor(collapse_leaves_factor) {
  }
  ~RegressionTree();

  void fit(RTNodeHistogram *hist,
           size_t *sampleids,
           float max_features);

  double update_output(double const *pseudoresponses);

  double update_output(double const *pseudoresponses,
                       double const *cachedweights);

  RTNode *get_proot() const {
    return root;
  }

 private:
  //if require_devianceltparent is true the node is split if minvar is lt the current node deviance (require_devianceltparent=false in RankLib)
  bool split(RTNode *node, const float max_features,
             const bool require_devianceltparent);

  size_t inline tree_heap_nodes(rt_maxheap_enriched& heap, RTNode* node,
                                size_t depth, double max_deviance);

};