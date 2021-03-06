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
#include "learning/forests/lambdamartselective.h"

#include <fstream>
#include <iomanip>
#include <chrono>
#include <random>

namespace quickrank {
namespace learning {
namespace forests {

const std::string LambdaMartSelective::NAME_ = "LAMBDAMART-SELECTIVE";


void
LambdaMartSelective::init(
    std::shared_ptr<quickrank::data::VerticalDataset> training_dataset) {
  LambdaMart::init(training_dataset);
}

void LambdaMartSelective::clear(size_t num_features) {
  LambdaMart::clear(num_features);
}

void LambdaMartSelective::learn(std::shared_ptr<quickrank::data::Dataset> training_dataset,
                 std::shared_ptr<quickrank::data::Dataset> validation_dataset,
                 std::shared_ptr<quickrank::metric::ir::Metric> scorer,
                 size_t partial_save, const std::string output_basename) {
  // ---------- Initialization ----------
  std::cout << "# Initialization";
  std::cout.flush();

  std::chrono::high_resolution_clock::time_point chrono_init_start =
      std::chrono::high_resolution_clock::now();

  // create a copy of the training datasets and put it in vertical format
  std::shared_ptr<quickrank::data::VerticalDataset> vertical_training(
      new quickrank::data::VerticalDataset(training_dataset));

  best_metric_on_validation_ = std::numeric_limits<double>::lowest();
  best_metric_on_training_ = std::numeric_limits<double>::lowest();
  best_model_ = 0;

  ensemble_model_.set_capacity(ntrees_);

  init(vertical_training);

  if (validation_dataset) {
    scores_on_validation_ = new Score[validation_dataset->num_instances()]();
  }

  // if the ensemble size is greater than zero, it means the learn method has
  // to start not from scratch but from a previously saved (intermediate) model
  if (ensemble_model_.is_notempty()) {
    best_model_ = ensemble_model_.get_size() - 1;

    // Update the model's outputs on all training samples
    score_dataset(training_dataset, scores_on_training_);
    // run metric
    best_metric_on_training_ = scorer->evaluate_dataset(
        vertical_training, scores_on_training_);

    if (validation_dataset) {
      // Update the model's outputs on all validation samples
      score_dataset(validation_dataset, scores_on_validation_);
      // run metric
      best_metric_on_validation_ = scorer->evaluate_dataset(
          validation_dataset, scores_on_validation_);
    }
  }

  auto chrono_init_end = std::chrono::high_resolution_clock::now();
  double init_time = std::chrono::duration_cast<std::chrono::duration<double>>(
      chrono_init_end - chrono_init_start).count();
  std::cout << ": " << std::setprecision(2) << init_time << " s." << std::endl;

  // ---------- Training ----------
  std::cout << std::fixed << std::setprecision(4);

  std::cout << "# Training:" << std::endl;
  std::cout << "# -------------------------" << std::endl;
  std::cout << "# iter. training validation" << std::endl;
  std::cout << "# -------------------------" << std::endl;

  // Used for document sampling and node splitting
  size_t nsampleids = training_dataset->num_instances();
  size_t *sampleids = new size_t[nsampleids];
  size_t *sampleids_orig = NULL;
  size_t *npositives = NULL;
  size_t nsampleids_iter = nsampleids;
  bool *sample_presence = NULL;

  if (rank_sampling_factor > 0 || random_sampling_factor > 0)
    sample_presence = new bool[nsampleids];

  // If we do not use document sampling, we fill the sampleids only once
  #pragma omp parallel for
  for (size_t i = 0; i < nsampleids; ++i) {
    sampleids[i] = i;
    if (sample_presence != NULL)
      sample_presence[i] = true;
  }

  if (rank_sampling_factor > 0 || random_sampling_factor > 0) {
    sampleids_orig = new size_t[nsampleids];
    memcpy(sampleids_orig, sampleids, sizeof(size_t) * nsampleids);

    npositives = new size_t[training_dataset->num_queries()];
    #pragma omp parallel for
    for (size_t q=0; q<training_dataset->num_queries(); ++q) {

      size_t start_offset = training_dataset->offset(q);
      size_t end_offset = training_dataset->offset(q + 1);

      size_t num_pos = 0;
      for (size_t d = start_offset; d < end_offset; ++d) {
        if (training_dataset->getLabel(d) > 0)
          ++num_pos;
      }

      npositives[q] = num_pos;
    }
  }

  // shows the performance of the already trained model..
  if (ensemble_model_.is_notempty()) {
    std::cout << std::setw(7) << ensemble_model_.get_size()
              << std::setw(9) << best_metric_on_training_;

    if (validation_dataset)
      std::cout << std::setw(9) << best_metric_on_validation_;

    std::cout << " *" << std::endl;
  }

  auto chrono_train_start = std::chrono::high_resolution_clock::now();

  /* initialize random seed: */
  srand(0);
//      srand(time(NULL));

  // start iterations from 0 or (ensemble_size - 1)
  std::vector<bool> improvements((int) normalization_factor, true);
  float adapt_factor = 1;
  for (size_t m = ensemble_model_.get_size(); m < ntrees_; ++m) {
    if (validation_dataset
        && (valid_iterations_ && m > best_model_ + valid_iterations_))
      break;

    if ((rank_sampling_factor > 0 || random_sampling_factor > 0) &&
        m % sampling_iterations == 0 && m > 0) {

      // Reset sampleids and reorder on a query basis
      memcpy(sampleids, sampleids_orig, sizeof(size_t) * nsampleids);
      nsampleids_iter = sampling_query_level(training_dataset,
                                             sampleids,
                                             npositives,
                                             adapt_factor);

      std::cout << "Reducing training size from "
                << nsampleids << " to "
                << nsampleids_iter << std::endl;
    }

    if (subsample_ != 1.0f) {

      // shuffle the sample idx with the current sample size (previous sampling)
      auto seed = std::chrono::system_clock::now().time_since_epoch().count();
      auto rng = std::default_random_engine(seed);
      std::shuffle(&sampleids[0],
                   &sampleids[nsampleids_iter],
                   rng);

      if (subsample_ > 1.0f) {
        // >1: Max feature is the number of features to use
        nsampleids_iter = (size_t) std::min((size_t) subsample_, nsampleids_iter);
      } else {
        // <1: Max feature is the fraction of features to use
        nsampleids_iter = (size_t) std::floor(subsample_ * nsampleids_iter);
      }
    }

    // If we are training on a sample of the full dataset, we need to update
    // the presence map
    if (nsampleids_iter < nsampleids) {
//      #pragma omp parallel for
      for (size_t i=0; i<training_dataset->num_instances(); ++i) {
        sample_presence[sampleids[i]] = i < nsampleids_iter;
      }
    }

    compute_pseudoresponses(vertical_training, scorer.get(), sample_presence);

    // update the histogram with these training_setting labels
    // (the feature histogram will be used to find the best tree rtnode)
    hist_->update(pseudoresponses_, nsampleids_iter, sampleids);

    // Fit a regression tree
    std::unique_ptr<RegressionTree> tree =
        fit_regressor_on_gradient(vertical_training, sampleids);

    //add this tree to the ensemble (our model)
    ensemble_model_.push(tree->get_proot(), shrinkage_, 0);  // maxlabel);

    //Update the model's outputs on all training samples
    update_modelscores(vertical_training, scores_on_training_, tree.get());
    // run metric
    quickrank::MetricScore metric_on_training = scorer->evaluate_dataset(
        vertical_training, scores_on_training_);

    //show results
    std::cout << std::setw(7) << m + 1 << std::setw(9) << metric_on_training;

    //Evaluate the current model on the validation data (if available)
    if (validation_dataset) {
      // update validation scores
      update_modelscores(validation_dataset, scores_on_validation_, tree.get());

      // run metric
      quickrank::MetricScore metric_on_validation = scorer->evaluate_dataset(
          validation_dataset, scores_on_validation_);
      std::cout << std::setw(9) << metric_on_validation;

      if (metric_on_validation > best_metric_on_validation_) {
        best_metric_on_training_ = metric_on_training;
        best_metric_on_validation_ = metric_on_validation;
        best_model_ = ensemble_model_.get_size() - 1;
        std::cout << " *";
      }

    } else {
      if (metric_on_training > best_metric_on_training_) {
        best_metric_on_training_ = metric_on_training;
        best_model_ = ensemble_model_.get_size() - 1;
        std::cout << " *";
      }
    }
    std::cout << std::endl;

    if (adaptive_strategy != "NO" && normalization_factor > 0) {
      // Rank/Random factor adaptability depending from last iter with improv.
      improvements[m % improvements.size()] =
          best_model_ == (ensemble_model_.get_size() - 1);

      float iters_improvement = (float) std::accumulate(improvements.begin(),
                                                        improvements.end(),
                                                        0.0);
      adapt_factor = iters_improvement / improvements.size();
    }

    if (partial_save != 0 and !output_basename.empty()
        and (m + 1) % partial_save == 0) {
      save(output_basename, m + 1);
    }
  }

  delete[] sampleids;
  if (sampleids_orig)
    delete[] sampleids_orig;
  if (npositives)
    delete[] npositives;
  if (sample_presence)
    delete[] sample_presence;

  //Rollback to the best model observed on the validation data
  if (validation_dataset) {
    while (ensemble_model_.is_notempty()
        && ensemble_model_.get_size() > best_model_ + 1) {
      ensemble_model_.pop();
    }
  }

  auto chrono_train_end = std::chrono::high_resolution_clock::now();
  double train_time = std::chrono::duration_cast<std::chrono::duration<double>>(
      chrono_train_end - chrono_train_start).count();

  //Finishing up
  std::cout << std::endl;
  std::cout << *scorer << " on training data = " << best_metric_on_training_
            << std::endl;

  if (validation_dataset) {
    std::cout << *scorer << " on validation data = "
              << best_metric_on_validation_ << std::endl;
  }

  clear(vertical_training->num_features());

  std::cout << std::endl;
  std::cout << "#\t Training Time: " << std::setprecision(2) << train_time
            << " s." << std::endl;
}

std::ostream &LambdaMartSelective::put(std::ostream &os) const {
  Mart::put(os);
  os << "# sampling iterations = " << sampling_iterations << std::endl;
  os << "# rank sampling factor = " << rank_sampling_factor << std::endl;
  os << "# random sampling factor = " << random_sampling_factor << std::endl;
  os << "# normalization factor = " << normalization_factor << std::endl;
  os << "# adaptive strategy = " << adaptive_strategy << std::endl;
  os << "# negative strategy = " << negative_strategy << std::endl;
  return os;
}

size_t LambdaMartSelective::sampling_query_level(
    std::shared_ptr<data::Dataset> dataset,
    size_t *sampleids,
    size_t *npositives,
    float adapt_factor) {

  if (!sampling_iterations)
    return dataset->num_instances();

  float sum_factors = rank_sampling_factor + random_sampling_factor;
//  if (sum_factors > 1)
//    sum_factors = 1;

  float rank_factor, random_factor;
  float inv_adapt_factor = 1 - adapt_factor;

  if (adaptive_strategy == "NO") {

    rank_factor = rank_sampling_factor;
    random_factor = random_sampling_factor;

  } else if (adaptive_strategy == "FIXED") {

    auto min_ratio = fmin(rank_sampling_factor, random_sampling_factor);
    auto max_ratio = fmax(rank_sampling_factor, random_sampling_factor);
    auto delta = max_ratio - min_ratio;
    rank_factor = random_factor = min_ratio + inv_adapt_factor * delta;

  } else if (adaptive_strategy == "RATIO") {

    rank_factor = sum_factors * adapt_factor;
    random_factor = sum_factors - rank_factor;

  } else if (adaptive_strategy == "MIX") {

    auto min_ratio = fmin(rank_sampling_factor, random_sampling_factor);
    auto max_ratio = fmax(rank_sampling_factor, random_sampling_factor);
    auto delta = max_ratio - min_ratio;
    auto factor = min_ratio + inv_adapt_factor * delta;

    rank_factor = factor * adapt_factor;
    random_factor = factor - rank_factor;
  }

  std::cout << "Rank Factor: " << rank_factor
            << " - Random Factor: " << random_factor
            << " - Adapt Factor: " << adapt_factor
            << std::setprecision(4) << std::endl;

  size_t cursor = 0;
  size_t neg_sel_rank = 0;
  size_t neg_sel_random = 0;
  size_t n_pos = 0;
  for (size_t q=0; q<dataset->num_queries(); ++q) {

    size_t start_offset = dataset->offset(q);
    size_t end_offset = dataset->offset(q + 1);
    size_t query_size = end_offset - start_offset;

    size_t n_neg_query = query_size - npositives[q];
    size_t n_top_neg, n_random_neg;

    if (negative_strategy == "RATIO") {
      n_top_neg = (size_t) std::round(rank_factor * n_neg_query);
      n_random_neg = (size_t) std::round(random_factor * n_neg_query);

    } else if (negative_strategy == "MUL") {
      n_top_neg = (size_t) std::round(rank_factor * npositives[q]);
      n_random_neg = (size_t) std::round(random_factor * npositives[q]);
      n_top_neg = std::min(n_top_neg, n_neg_query);
      n_random_neg = std::min(n_random_neg, n_neg_query);

    } else if (negative_strategy == "POS") {
      if (npositives[q] == 0) {

        n_top_neg = 0;
        n_random_neg = 0;

      } else {

        std::sort(&sampleids[start_offset], &sampleids[end_offset],
                  [this](size_t i1, size_t i2) {
                    return scores_on_training_[i1] > scores_on_training_[i2];
                  });

        size_t last_pos = 0;
        #pragma omp parallel for reduction(max : last_pos)
        for (size_t i=0; i<query_size; ++i) {
          if (dataset->getLabel(sampleids[start_offset + i]) > 0) {
            last_pos = i;
          }
        }

        size_t n_neg_before_last_pos = last_pos - npositives[q] + 1;
        n_top_neg = std::min(
            (size_t) std::round(rank_factor *n_neg_before_last_pos),
            n_neg_query);
        n_random_neg = std::min(
            (size_t) std::round(random_factor * n_neg_before_last_pos),
            n_neg_query - n_top_neg);

//        std::cout << "Position Last positive: " << last_pos
//                  << " - n_neg_before_last_pos: " << n_neg_before_last_pos
//                  << std::endl;
      }
    } else {
      throw std::logic_error("Not supported!");
    }

    size_t n_total_neg = n_top_neg + n_random_neg;
    if (n_total_neg > n_neg_query) {
      n_total_neg = n_neg_query;
      n_random_neg = n_neg_query - n_top_neg;
    }

    neg_sel_rank += n_top_neg;
    neg_sel_random += n_random_neg;
    n_pos += npositives[q];

//    std::cout << std::setprecision(0)
//              << "Query: " << q
//              << " - Size: " << query_size
//              << " - N. Pos: " << npositives[q]
//              << " - N. Neg: " << n_neg_query
//              << " - n_top_neg: " << n_top_neg
//              << " - n_random_neg: " << n_random_neg
//              << std::setprecision(4) << std::endl;

    std::sort(&sampleids[start_offset], &sampleids[end_offset],
              [this, &dataset](size_t i1, size_t i2) {

                bool grt = scores_on_training_[i1] > scores_on_training_[i2];

                return dataset->getLabel(i1) > 0 ?
                        dataset->getLabel(i2) == 0 || grt :
                        dataset->getLabel(i2) == 0 && grt;
              });

    if (cursor > 0) {
      for (size_t j = 0; j < npositives[q] + n_top_neg; ++j) {
        std::swap(sampleids[cursor + j],
                  sampleids[start_offset + j]);
      }
    }

    if (n_random_neg > 0) {

      std::vector<int> indices(query_size - npositives[q] - n_top_neg);
      std::iota(indices.begin(), indices.end(), npositives[q] + n_top_neg);
      std::random_shuffle(indices.begin(), indices.end());

      for (size_t j=0; j<n_random_neg; ++j) {
        std::swap(sampleids[cursor + npositives[q] + n_top_neg + j],
                  sampleids[start_offset + indices[j]]);
      }
    }

    cursor += npositives[q] + n_total_neg;
  }

  std::cout << std::setprecision(0)
            << "N. Positives: " << n_pos
            << " - Neg sel rank: " << neg_sel_rank
            << " - Neg sel random: " << neg_sel_random
            << std::setprecision(4) << std::endl;

  return cursor;
}

}  // namespace forests
}  // namespace learning
}  // namespace quickrank
