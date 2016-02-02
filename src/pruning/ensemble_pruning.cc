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
 * Contributors:
 *  - Salvatore Trani (salvatore.trani@isti.cnr.it)
 */
#include "pruning/ensemble_pruning.h"

#include <fstream>
#include <iomanip>
#include <chrono>
#include <set>

#include <boost/property_tree/xml_parser.hpp>
#include <io/svml.h>

namespace quickrank {
namespace pruning {

const std::string EnsemblePruning::NAME_ = "EPRUNING";

const std::vector<std::string> EnsemblePruning::pruningMethodName = {
  "RANDOM", "LOW_WEIGHTS", "SKIP", "LAST", "QUALITY_LOSS", "SCORE_LOSS"
};

EnsemblePruning::EnsemblePruning(PruningMethod pruning_method,
                                 double pruning_rate)
    : pruning_rate_(pruning_rate),
      pruning_method_(pruning_method),
      lineSearch_() {
}

EnsemblePruning::EnsemblePruning(std::string pruning_method,
                                 double pruning_rate) :
    pruning_rate_(pruning_rate),
    pruning_method_(getPruningMethod(pruning_method)),
    lineSearch_() {
}

EnsemblePruning::EnsemblePruning(std::string pruning_method,
                                 double pruning_rate,
                                 std::shared_ptr<learning::linear::LineSearch> lineSearch) :
    pruning_rate_(pruning_rate),
    pruning_method_(getPruningMethod(pruning_method)),
    lineSearch_(lineSearch) {
}

EnsemblePruning::EnsemblePruning(const boost::property_tree::ptree &info_ptree,
                                 const boost::property_tree::ptree &model_ptree)
{
  pruning_rate_ = info_ptree.get <double> ("pruning-rate");
  auto pruning_method_name = info_ptree.get <std::string> ("pruning-method");
  pruning_method_ = getPruningMethod(pruning_method_name);

  unsigned int max_feature = 0;
  for (const boost::property_tree::ptree::value_type& tree: model_ptree) {
    if (tree.first == "tree") {
      unsigned int feature = tree.second.get<unsigned int>("index");
      if (feature > max_feature) {
        max_feature = feature;
      }
    }
  }

  estimators_to_prune_ = 0;
  std::vector<double>(max_feature, 0.0).swap(weights_);
  for (const boost::property_tree::ptree::value_type& tree: model_ptree) {
    if (tree.first == "tree") {
      int feature = tree.second.get<int>("index");
      double weight = tree.second.get<double>("weight");
      weights_[feature - 1] = weight;
      if (weight > 0)
        estimators_to_prune_++;
    }
  }
}

EnsemblePruning::~EnsemblePruning() {
}

std::ostream& EnsemblePruning::put(std::ostream &os) const {
  os << "# Ranker: " << name() << std::endl
    << "# pruning rate = " << pruning_rate_ << std::endl
    << "# pruning method = " << getPruningMethod(pruning_method_) << std::endl;
  if (lineSearch_)
    os << "# Line Search Parameters: " << std::endl << *lineSearch_;
  else
    os << "# No Line Search" << std::endl;
  return os << std::endl;
}

void EnsemblePruning::preprocess_dataset(
    std::shared_ptr<data::Dataset> dataset) const {

  if (dataset->format() != data::Dataset::HORIZ)
    dataset->transpose();
}

void EnsemblePruning::learn(
    std::shared_ptr<quickrank::data::Dataset> training_dataset,
    std::shared_ptr<quickrank::data::Dataset> validation_dataset,
    std::shared_ptr<quickrank::metric::ir::Metric> scorer,
    unsigned int partial_save, const std::string output_basename) {

  auto begin = std::chrono::steady_clock::now();

  // Do some initialization
  preprocess_dataset(training_dataset);
  if (validation_dataset)
    preprocess_dataset(validation_dataset);

  if (pruning_rate_ < 1)
    estimators_to_prune_ = (unsigned int) round(
        pruning_rate_ * training_dataset->num_features() );
  else {
    estimators_to_prune_ = pruning_rate_;
    if (estimators_to_prune_ >= training_dataset->num_features()) {
      std::cout << "Impossible to prune everything. Quit!" << std::endl;
      return;
    }
  }

  estimators_to_select_ =
      training_dataset->num_features() - estimators_to_prune_;

  // Set all the weights to 1 (and initialize the vector)
  std::vector<double>(training_dataset->num_features(), 1.0).swap(weights_);

  // compute training and validation scores using starting weights
  std::vector<Score> training_score(training_dataset->num_instances());
  score(training_dataset.get(), &training_score[0]);
  auto init_metric_on_training = scorer->evaluate_dataset(training_dataset,
                                                     &training_score[0]);

  std::cout << std::endl;
  std::cout << "# Without pruning:" << std::endl;
  std::cout << std::fixed << std::setprecision(4);
  std::cout << "# --------------------------" << std::endl;
  std::cout << "#       training validation" << std::endl;
  std::cout << "# --------------------------" << std::endl;
  std::cout << std::setw(16) << init_metric_on_training;
  if (validation_dataset) {
    std::vector<Score> validation_score(validation_dataset->num_instances());
    score(validation_dataset.get(), &validation_score[0]);
    auto init_metric_on_validation = scorer->evaluate_dataset(
        validation_dataset, &validation_score[0]);
    std::cout << std::setw(9) << init_metric_on_validation << std::endl;
  }
  std::cout << std::endl;

  std::set<unsigned int> pruned_estimators;

  // Some pruning methods needs to perform line search before the pruning
  if (pruning_method_ == PruningMethod::LOW_WEIGHTS ||
      pruning_method_ == PruningMethod::QUALITY_LOSS ||
      pruning_method_ == PruningMethod::SCORE_LOSS) {

    if (!lineSearch_) {
      throw std::invalid_argument(std::string(
          "This pruning method requires line search"));
    }

    if (lineSearch_->get_weigths().empty()) {

      // Need to do the line search pre pruning. The line search model is empty
      std::cout << "# LineSearch pre-pruning:" << std::endl;
      std::cout << "# --------------------------" << std::endl;
      lineSearch_->learn(training_dataset, validation_dataset, scorer,
                         partial_save, output_basename);
    } else {
      // The line search pre pruning is already done and the weights are in
      // the model. We just need to load them.
      std::cout << "# LineSearch pre-pruning already done:" << std::endl;
      std::cout << "# --------------------------" << std::endl;
    }

    // Needs to import the line search learned weights into this model
    import_weights_from_line_search(pruned_estimators);
    std::cout << std::endl;
  }

  switch (pruning_method_) {
    case PruningMethod::RANDOM: {
      random_pruning(pruned_estimators);
      break;
    }
    case PruningMethod::LOW_WEIGHTS: {
      low_weights_pruning(pruned_estimators);
      break;
    }
    case PruningMethod::LAST: {
      last_pruning(pruned_estimators);
      break;
    }
    case PruningMethod::QUALITY_LOSS: {
      quality_loss_pruning(pruned_estimators, training_dataset, scorer);
      break;
    }
    case PruningMethod::SKIP: {
      skip_pruning(pruned_estimators);
      break;
    }
    case PruningMethod::SCORE_LOSS: {
      score_loss_pruning(pruned_estimators, training_dataset);
      break;
    }
    default:
      throw std::invalid_argument("pruning method still not implemented");
  }

  // Set the weights of the pruned features to 0
  for (unsigned int f: pruned_estimators) {
    weights_[f] = 0;
  }

  if (lineSearch_) {

    // Filter the dataset by deleting the weight-0 features
    std::shared_ptr<data::Dataset> filtered_training_dataset;
    std::shared_ptr<data::Dataset> filtered_validation_dataset;

    filtered_training_dataset = filter_dataset(training_dataset,
                                               pruned_estimators);
    if (validation_dataset)
      filtered_validation_dataset = filter_dataset(validation_dataset,
                                                   pruned_estimators);

    // Run the line search algorithm
    std::cout << "# LineSearch post-pruning:" << std::endl;
    std::cout << "# --------------------------" << std::endl;
    // On each learn call, line search internally resets the weights vector
    lineSearch_->learn(filtered_training_dataset, filtered_validation_dataset,
                       scorer, partial_save, output_basename);
    std::cout << std::endl;

    // Needs to import the line search learned weights into this model
    import_weights_from_line_search(pruned_estimators);

  }

  score(training_dataset.get(), &training_score[0]);
  init_metric_on_training = scorer->evaluate_dataset(training_dataset,
                                                     &training_score[0]);

  std::cout << "# With pruning:" << std::endl;
  std::cout << std::fixed << std::setprecision(4);
  std::cout << "# --------------------------" << std::endl;
  std::cout << "#       training validation" << std::endl;
  std::cout << "# --------------------------" << std::endl;
  std::cout << std::setw(16) << init_metric_on_training;
  if (validation_dataset) {
    std::vector<Score> validation_score(validation_dataset->num_instances());
    score(validation_dataset.get(), &validation_score[0]);
    auto init_metric_on_validation = scorer->evaluate_dataset(
        validation_dataset, &validation_score[0]);
    std::cout << std::setw(9) << init_metric_on_validation << std::endl;
  }

  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double> elapsed = std::chrono::duration_cast<
      std::chrono::duration<double>>(end - begin);
  std::cout << std::endl;
  std::cout << "# \t Total training time: " << std::setprecision(2) <<
      elapsed.count() << " seconds" << std::endl;
}

Score EnsemblePruning::score_document(const Feature *d,
                                      const unsigned int next_fx_offset) const {
  // next_fx_offset is ignored as it is equal to 1 for horizontal dataset
  Score score = 0;
  for (unsigned int k = 0; k < weights_.size(); k++) {
    score += weights_[k] * d[k];
  }
  return score;
}

std::ofstream& EnsemblePruning::save_model_to_file(std::ofstream &os) const {
  // write ranker description
  os << "\t<info>" << std::endl;
  os << "\t\t<type>" << name() << "</type>" << std::endl;
  os << "\t\t<pruning-method>" << getPruningMethod(pruning_method_) <<
      "</pruning-method>" << std::endl;
  os << "\t\t<pruning-rate>" << pruning_rate_ << "</pruning-rate>" << std::endl;
  os << "\t</info>" << std::endl;

  os << "\t<ensemble>" << std::endl;
  auto old_precision = os.precision();
  os.setf(std::ios::floatfield, std::ios::fixed);
  for (unsigned int i = 0; i < weights_.size(); i++) {
    os << "\t\t<tree>" << std::endl;
    os << std::setprecision(3);
    os << "\t\t\t<index>" << i + 1 << "</index>" << std::endl;
    os << std::setprecision(std::numeric_limits<Score>::max_digits10);
    os << "\t\t\t<weight>" << weights_[i] << "</weight>" <<
    std::endl;
    os << "\t\t</tree>" << std::endl;
  }
  os << "\t</ensemble>" << std::endl;
  os << std::setprecision(old_precision);
  return os;
}

void EnsemblePruning::score(data::Dataset *dataset, Score *scores) const {

  Feature* features = dataset->at(0,0);
  #pragma omp parallel for
  for (unsigned int s = 0; s < dataset->num_instances(); s++) {
    unsigned int offset_feature = s * dataset->num_features();
    scores[s] = 0;
    // compute feature * weight for all the feature different from f
    for (unsigned int f = 0; f < dataset->num_features(); f++) {
      scores[s] += weights_[f] * features[offset_feature + f];
    }
  }
}

void EnsemblePruning::import_weights_from_line_search(
    std::set<unsigned int>& pruned_estimators) {

  std::vector<double> ls_weights = lineSearch_->get_weigths();

  unsigned int ls_f = 0;
  for (unsigned int f = 0; f < weights_.size(); f++) {
    if (!pruned_estimators.count(f)) // skip weights-0 features (pruned by ls)
      weights_[f] = ls_weights[ls_f++];
  }

  assert(ls_f == ls_weights.size());
}

std::shared_ptr<data::Dataset> EnsemblePruning::filter_dataset(
      std::shared_ptr<data::Dataset> dataset,
      std::set<unsigned int>& pruned_estimators) const {

  data::Dataset* filt_dataset = new data::Dataset(dataset->num_instances(),
                                                  estimators_to_select_);

  // allocate feature vector
  std::vector<Feature> featureSelected(estimators_to_select_);
  unsigned int skipped;

  if (dataset->format() == dataset->VERT)
    dataset->transpose();

  for (unsigned int q = 0; q < dataset->num_queries(); q++) {
    std::shared_ptr<data::QueryResults> results = dataset->getQueryResults(q);
    const Feature* features = results->features();
    const Label* labels = results->labels();

    for (unsigned int r = 0; r < results->num_results(); r++) {
      skipped = 0;
      for (unsigned int f = 0; f < dataset->num_features(); f++) {
        if (pruned_estimators.count(f)) {
          skipped++;
        } else {
          featureSelected[f - skipped] = features[f];
        }
      }
      features += dataset->num_features();
      filt_dataset->addInstance(q, labels[r], featureSelected);
    }
  }

  return std::shared_ptr<data::Dataset>(filt_dataset);
}

void EnsemblePruning::random_pruning(
    std::set<unsigned int>& pruned_estimators) {

  unsigned int num_features = (unsigned int) weights_.size();

  /* initialize random seed: */
  srand (time(NULL));

  while (pruned_estimators.size() < estimators_to_prune_) {
    unsigned int index = rand() % num_features;
    if (!pruned_estimators.count(index))
      pruned_estimators.insert(index);
  }
}

void EnsemblePruning::skip_pruning(std::set<unsigned int>& pruned_estimators) {

  unsigned int num_features = (unsigned int) weights_.size();
  double step = (double)num_features / estimators_to_select_;

  std::set<unsigned int> selected_estimators;
  for (unsigned int i = 0; i < estimators_to_select_; i++) {
    selected_estimators.insert( (unsigned int) ceil(i * step) );
  }

  for (unsigned int f = 0; f < num_features; f++) {
    if (!selected_estimators.count(f))
      pruned_estimators.insert(f);
  }
}

void EnsemblePruning::last_pruning(std::set<unsigned int>& pruned_estimators) {

  unsigned int num_features = (unsigned int) weights_.size();

  for (unsigned int i=1; i <= estimators_to_prune_; i++) {
    pruned_estimators.insert(num_features - i);
  }
}

void EnsemblePruning::low_weights_pruning(
    std::set<unsigned int>& pruned_estimators) {

  std::vector<unsigned int> idx (weights_.size());
  std::iota(idx.begin(), idx.end(), 0);
  std::sort(idx.begin(), idx.end(),
            [this] (const unsigned int& a, const unsigned int& b) {
              return this->weights_[a] < this->weights_[b];
            });

  for (unsigned int f = 0; f < estimators_to_prune_; f++) {
    pruned_estimators.insert(idx[f]);
  }
}

void EnsemblePruning::quality_loss_pruning(
    std::set<unsigned int>& pruned_estimators,
    std::shared_ptr<data::Dataset> dataset,
    std::shared_ptr<metric::ir::Metric> scorer) {

  unsigned int num_features = dataset->num_features();

  std::vector<MetricScore> metric_scores(num_features);
  std::vector<Score> dataset_score(dataset->num_instances());

  for (unsigned int f = 0; f < num_features; f++) {
    // set the weight of the feature to 0 to simulate its deletion
    double weight_bkp = weights_[f];
    weights_[f] = 0;

    score(dataset.get(), &dataset_score[0]);
    metric_scores[f] = scorer->evaluate_dataset(dataset, &dataset_score[0]);

    // Reset the original weight to the feature
    weights_[f] = weight_bkp;
  }

  // Find the last metric scores
  std::vector<unsigned int> idx (num_features);
  std::iota(idx.begin(), idx.end(), 0);
  std::sort(idx.begin(), idx.end(),
            [&metric_scores] (const unsigned int& a, const unsigned int& b) {
              return metric_scores[a] > metric_scores[b];
            });

  for (unsigned int f = 0; f < estimators_to_prune_; f++) {
    pruned_estimators.insert(idx[f]);
  }
}

void EnsemblePruning::score_loss_pruning(
    std::set<unsigned int>& pruned_estimators,
    std::shared_ptr<data::Dataset> dataset) {

  unsigned int num_features = dataset->num_features();
  unsigned int num_instances = dataset->num_instances();
  std::vector<Score> feature_scores(num_features, 0);

  Feature* features = dataset->at(0,0);
  #pragma omp parallel for
  for (unsigned int s = 0; s < num_instances; s++) {
    unsigned int offset_feature = s * num_features;
    for (unsigned int f = 0; f < num_features; f++) {
      feature_scores[f] += weights_[f] * features[offset_feature + f];
    }
  }

  // Find the last feature scores
  std::vector<unsigned int> idx (num_features);
  std::iota(idx.begin(), idx.end(), 0);
  std::sort(idx.begin(), idx.end(),
            [&feature_scores] (const unsigned int& a, const unsigned int& b) {
              return feature_scores[a] < feature_scores[b];
            });

  for (unsigned int f = 0; f < estimators_to_prune_; f++) {
    pruned_estimators.insert(idx[f]);
  }
}


}  // namespace pruning
}  // namespace quickrank
