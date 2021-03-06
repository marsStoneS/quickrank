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

#include "types.h"
#include "learning/forests/mart.h"
#include "learning/tree/rt.h"
#include "learning/tree/ensemble.h"

namespace quickrank {
namespace learning {
namespace forests {

class RandomForest: public Mart {
 public:
  /// Initializes a new RandomForest instance with the given learning
  /// parameters.
  ///
  /// \param ntrees Maximum number of trees.
  /// \param shrinkage Learning rate.
  /// \param nthresholds Number of bins in discretization. 0 means no discretization.
  /// \param ntreeleaves Maximum number of leaves in each tree.
  /// \param minleafsupport Minimum number of instances in each leaf.
  /// \param esr Early stopping if no improvement after \esr iterations
  /// on the validation set.
  RandomForest(size_t ntrees, double shrinkage, size_t nthresholds,
               size_t ntreeleaves, size_t minleafsupport, float subsample,
               float max_features, size_t esr, float collapse_leaves_factor)
      : Mart(ntrees, shrinkage, nthresholds, ntreeleaves, minleafsupport,
             subsample, max_features, esr, collapse_leaves_factor) {
  }

  /// Generates a LTR_Algorithm instance from a previously saved XML model.
  RandomForest(const pugi::xml_document &model) : Mart(model) {
  }

  virtual ~RandomForest() {
  }

  /// Returns the name of the ranker.
  virtual std::string name() const {
    return NAME_;
  }

  static const std::string NAME_;

 protected:
  /// Prepares private data structurs befor training takes place.
  virtual void init(std::shared_ptr<data::VerticalDataset> training_dataset);

  /// Computes pseudo responses.
  ///
  /// \param training_dataset The training data.
  /// \param metric The metric to be optimized.
  virtual void compute_pseudoresponses(
      std::shared_ptr<data::VerticalDataset> training_dataset,
      metric::ir::Metric *metric);
};

}  // namespace forests
}  // namespace learning
}  // namespace quickrank
