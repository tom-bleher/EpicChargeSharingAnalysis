#include "2DPowerLorentzianFitCeres.hh"
#include "CeresLoggingInit.hh"
#include "Constants.hh"
#include "G4SystemOfUnits.hh"

#include <cmath>
#include <algorithm>
#include <map>
#include <iostream>
#include <mutex>
#include <atomic>
#include <limits>
#include <numeric>

// Ceres Solver includes
#include "ceres/ceres.h"
#include "glog/logging.h"

// Thread-safe mutex for Ceres operations
static std::mutex gCeresPowerLorentzianFitMutex;
static std::atomic<int> gGlobalCeresPowerLorentzianFitCounter{0};

// Use shared Google logging initialization
void InitializeCeresPowerLorentzian() {
    CeresLoggingInitializer::InitializeOnce();
}

// Calculate uncertainty as 5% of max charge in line (if enabled) - same as Lorentzian
double CalculatePowerLorentzianUncertainty(double max_charge_in_line) {
    if (!Constants::ENABLE_VERTICAL_CHARGE_UNCERTAINTIES) {
        return 1.0; // Uniform weighting when uncertainties are disabled
    }
    
    // Uncertainty = 5% of max charge when enabled
    double uncertainty = 0.05 * max_charge_in_line;
    if (uncertainty < Constants::MIN_UNCERTAINTY_VALUE) uncertainty = Constants::MIN_UNCERTAINTY_VALUE; // Prevent division by zero
    return uncertainty;
}

// Power-Law Lorentzian cost function
// Function form: y(x) = A / (1 + ((x-m)/gamma)^2)^beta + B
struct PowerLorentzianCostFunction {
    PowerLorentzianCostFunction(double x, double y, double uncertainty) 
        : x_(x), y_(y), uncertainty_(uncertainty) {}
    
    template <typename T>
    bool operator()(const T* const params, T* residual) const {
        // params[0] = A (amplitude)
        // params[1] = m (center)
        // params[2] = gamma (width parameter, like HWHM)
        // params[3] = beta (power exponent)
        // params[4] = B (baseline)
        
        const T& A = params[0];
        const T& m = params[1];
        const T& gamma = params[2];
        const T& beta = params[3];
        const T& B = params[4];
        
        // Robust handling of gamma (prevent division by zero)
        T safe_gamma = ceres::abs(gamma);
        if (safe_gamma < T(1e-12)) {
            safe_gamma = T(1e-12);
        }
        
        // Ensure beta stability (positive values)
        T safe_beta = ceres::abs(beta);
        if (safe_beta < T(0.1)) {
            safe_beta = T(0.1);
        }
        
        // Power-Law Lorentzian function: y(x) = A / (1 + ((x - m) / γ)^2)^β + B
        T dx = x_ - m;
        T normalized_dx = dx / safe_gamma;
        T denominator_base = T(1.0) + normalized_dx * normalized_dx;
        
        // Prevent numerical issues with very small denominators
        if (denominator_base < T(1e-12)) {
            denominator_base = T(1e-12);
        }
        
        // Apply power exponent
        T denominator = ceres::pow(denominator_base, safe_beta);
        T predicted = A / denominator + B;
        
        // Residual divided by uncertainty (standard weighted least squares) - same as Lorentzian
        residual[0] = (predicted - T(y_)) / T(uncertainty_);
        
        return true;
    }
    
    static ceres::CostFunction* Create(double x, double y, double uncertainty) {
        return (new ceres::AutoDiffCostFunction<PowerLorentzianCostFunction, 1, 5>(
            new PowerLorentzianCostFunction(x, y, uncertainty)));
    }
    
private:
    const double x_;
    const double y_;
    const double uncertainty_;
};

// Robust statistics calculations with improved outlier detection
struct DataStatistics {
    double mean;
    double median;
    double std_dev;
    double mad; // Median Absolute Deviation
    double q25, q75; // Quartiles
    double min_val, max_val;
    double weighted_mean;
    double total_weight;
    double robust_center; // Improved center estimate
    bool valid;
};

DataStatistics CalculateRobustStatisticsPowerLorentzian(const std::vector<double>& x_vals, 
                                                      const std::vector<double>& y_vals) {
    DataStatistics stats;
    stats.valid = false;
    
    if (x_vals.size() != y_vals.size() || x_vals.empty()) {
        return stats;
    }
    
    // Basic statistics
    stats.min_val = *std::min_element(y_vals.begin(), y_vals.end());
    stats.max_val = *std::max_element(y_vals.begin(), y_vals.end());
    
    // Mean and standard deviation
    stats.mean = std::accumulate(y_vals.begin(), y_vals.end(), 0.0) / y_vals.size();
    
    double variance = 0.0;
    for (double val : y_vals) {
        variance += (val - stats.mean) * (val - stats.mean);
    }
    stats.std_dev = std::sqrt(variance / y_vals.size());
    
    // Median and quartiles
    std::vector<double> sorted_y = y_vals;
    std::sort(sorted_y.begin(), sorted_y.end());
    
    size_t n = sorted_y.size();
    if (n % 2 == 0) {
        stats.median = (sorted_y[n/2 - 1] + sorted_y[n/2]) / 2.0;
    } else {
        stats.median = sorted_y[n/2];
    }
    
    stats.q25 = sorted_y[n/4];
    stats.q75 = sorted_y[3*n/4];
    
    // Median Absolute Deviation
    std::vector<double> abs_deviations;
    for (double val : y_vals) {
        abs_deviations.push_back(std::abs(val - stats.median));
    }
    std::sort(abs_deviations.begin(), abs_deviations.end());
    stats.mad = abs_deviations[n/2] * 1.4826;
    
    // Numerical stability safeguard
    if (!std::isfinite(stats.mad) || stats.mad < 1e-12) {
        stats.mad = (std::isfinite(stats.std_dev) && stats.std_dev > 1e-12) ?
                    stats.std_dev : 1e-12;
    }
    
    // Use same center estimation logic as Lorentzian for consistency
    stats.weighted_mean = 0.0;
    stats.total_weight = 0.0;
    
    for (size_t i = 0; i < x_vals.size(); ++i) {
        double weight = std::max(0.0, y_vals[i] - stats.q25);
        if (weight > 0) {
            stats.weighted_mean += x_vals[i] * weight;
            stats.total_weight += weight;
        }
    }
    
    if (stats.total_weight > 0) {
        stats.weighted_mean /= stats.total_weight;
        stats.robust_center = stats.weighted_mean;
    } else {
        // Fallback to arithmetic mean
        stats.weighted_mean = std::accumulate(x_vals.begin(), x_vals.end(), 0.0) / x_vals.size();
        stats.robust_center = stats.weighted_mean;
    }
    
    stats.valid = true;
    return stats;
}

// Parameter estimation structures for Power-Law Lorentzian
struct PowerLorentzianParameterEstimates {
    double amplitude;
    double center;
    double gamma;
    double beta;
    double baseline;
    double amplitude_err;
    double center_err;
    double gamma_err;
    double beta_err;
    double baseline_err;
    bool valid;
    int method_used;
};

// Parameter estimation for Power-Law Lorentzian distributions
PowerLorentzianParameterEstimates EstimatePowerLorentzianParameters(
    const std::vector<double>& x_vals,
    const std::vector<double>& y_vals,
    double center_estimate,
    double pixel_spacing,
    bool verbose = false) {
    
    PowerLorentzianParameterEstimates estimates;
    estimates.valid = false;
    estimates.method_used = 0;
    
    if (x_vals.size() != y_vals.size() || x_vals.size() < 5) {
        return estimates;
    }
    
    DataStatistics stats = CalculateRobustStatisticsPowerLorentzian(x_vals, y_vals);
    if (!stats.valid) {
        return estimates;
    }
    
    if (verbose) {
        std::cout << "Power Lorentzian data statistics: min=" << stats.min_val << ", max=" << stats.max_val 
                 << ", median=" << stats.median << ", weighted_mean=" << stats.weighted_mean << std::endl;
    }
    
    // Method 1: Physics-based estimation for charge distributions
    estimates.center = stats.weighted_mean;
    estimates.baseline = std::min(stats.min_val, stats.q25);
    estimates.amplitude = stats.max_val - estimates.baseline;
    estimates.beta = 1.0; // Start with standard Lorentzian
    
    // For Power Lorentzian: gamma estimation based on charge spread (similar to Lorentzian)
    double distance_spread = 0.0;
    double weight_sum = 0.0;
    
    for (size_t i = 0; i < x_vals.size(); ++i) {
        double weight = std::max(0.0, y_vals[i] - estimates.baseline);
        if (weight > 0.1 * estimates.amplitude) {
            double dx = x_vals[i] - estimates.center;
            distance_spread += weight * dx * dx;
            weight_sum += weight;
        }
    }
    
    if (weight_sum > 0) {
        // For Power Lorentzian, gamma ≈ sqrt(2*sigma^2) where sigma is from Gaussian equivalent
        estimates.gamma = std::sqrt(2.0 * distance_spread / weight_sum);
    } else {
        estimates.gamma = pixel_spacing * 0.7; // Larger default for Power Lorentzian
    }
    
    // Apply physics-based bounds (Power Lorentzian has wider tails)
    estimates.gamma = std::max(pixel_spacing * 0.3, std::min(pixel_spacing * 3.0, estimates.gamma));
    estimates.amplitude = std::max(estimates.amplitude, (stats.max_val - stats.min_val) * 0.1);
    
    // Validate Method 1
    if (estimates.amplitude > 0 && estimates.gamma > 0 && 
        !std::isnan(estimates.center) && !std::isnan(estimates.amplitude) && 
        !std::isnan(estimates.gamma) && !std::isnan(estimates.beta) && !std::isnan(estimates.baseline)) {
        estimates.method_used = 1;
        estimates.valid = true;
        
        if (verbose) {
            std::cout << "Power Lorentzian Method 1 (Physics-based): A=" << estimates.amplitude 
                     << ", m=" << estimates.center << ", gamma=" << estimates.gamma 
                     << ", beta=" << estimates.beta << ", B=" << estimates.baseline << std::endl;
        }
        return estimates;
    }
    
    // Method 2: Robust statistical estimation
    estimates.center = stats.median;
    estimates.baseline = stats.q25;
    estimates.amplitude = stats.q75 - stats.q25;
    estimates.gamma = std::max(stats.mad, pixel_spacing * 0.5);
    estimates.beta = 1.0;
    
    if (estimates.amplitude > 0 && estimates.gamma > 0) {
        estimates.method_used = 2;
        estimates.valid = true;
        
        if (verbose) {
            std::cout << "Power Lorentzian Method 2 (Robust statistical): A=" << estimates.amplitude 
                     << ", m=" << estimates.center << ", gamma=" << estimates.gamma 
                     << ", beta=" << estimates.beta << ", B=" << estimates.baseline << std::endl;
        }
        return estimates;
    }
    
    // Method 3: Conservative fallback
    estimates.center = center_estimate;
    estimates.baseline = 0.0;
    estimates.amplitude = stats.max_val;
    estimates.gamma = pixel_spacing * 0.7;
    estimates.beta = 1.0;
    estimates.method_used = 3;
    estimates.valid = true;
    
    if (verbose) {
        std::cout << "Power Lorentzian Method 3 (Conservative fallback): A=" << estimates.amplitude 
                 << ", m=" << estimates.center << ", gamma=" << estimates.gamma 
                 << ", beta=" << estimates.beta << ", B=" << estimates.baseline << std::endl;
    }
    
    return estimates;
}

// Outlier filtering for Power Lorentzian fitting (adapted from Lorentzian version)
std::pair<std::vector<double>, std::vector<double>> FilterPowerLorentzianOutliers(
    const std::vector<double>& x_vals,
    const std::vector<double>& y_vals,
    double sigma_threshold = 2.5,
    bool verbose = false) {
    
    std::vector<double> filtered_x, filtered_y;
    
    if (x_vals.size() != y_vals.size() || x_vals.size() < 5) {
        return std::make_pair(filtered_x, filtered_y);
    }
    
    DataStatistics stats = CalculateRobustStatisticsPowerLorentzian(x_vals, y_vals);
    if (!stats.valid) {
        return std::make_pair(x_vals, y_vals);
    }
    
    // Use MAD-based outlier detection
    double outlier_threshold = stats.median + sigma_threshold * stats.mad;
    double lower_threshold = stats.median - sigma_threshold * stats.mad;
    
    int outliers_removed = 0;
    for (size_t i = 0; i < y_vals.size(); ++i) {
        if (y_vals[i] >= lower_threshold && y_vals[i] <= outlier_threshold) {
            filtered_x.push_back(x_vals[i]);
            filtered_y.push_back(y_vals[i]);
        } else {
            outliers_removed++;
        }
    }
    
    // Use lenient filtering if too many outliers removed
    if (filtered_x.size() < x_vals.size() / 2) {
        if (verbose) {
            std::cout << "Too many Power Lorentzian outliers detected (" << outliers_removed 
                     << "), using lenient filtering" << std::endl;
        }
        
        filtered_x.clear();
        filtered_y.clear();
        
        double extreme_threshold = stats.median + 4.0 * stats.mad;
        double extreme_lower = stats.median - 4.0 * stats.mad;
        
        for (size_t i = 0; i < y_vals.size(); ++i) {
            if (y_vals[i] >= extreme_lower && y_vals[i] <= extreme_threshold) {
                filtered_x.push_back(x_vals[i]);
                filtered_y.push_back(y_vals[i]);
            }
        }
    }
    
    if (filtered_x.size() < 5) {
        if (verbose) {
            std::cout << "Warning: After Power Lorentzian outlier filtering, only " << filtered_x.size() 
                     << " points remain" << std::endl;
        }
        return std::make_pair(x_vals, y_vals);
    }
    
    if (verbose && outliers_removed > 0) {
        std::cout << "Removed " << outliers_removed << " Power Lorentzian outliers, " 
                 << filtered_x.size() << " points remaining" << std::endl;
    }
    
    return std::make_pair(filtered_x, filtered_y);
}

// Core Power-Law Lorentzian fitting function using Ceres Solver
// Model: y(x) = A / (1 + ((x-m)/gamma)^2)^beta + B
bool FitPowerLorentzianCeres(
    const std::vector<double>& x_vals,
    const std::vector<double>& y_vals,
    double center_estimate,
    double pixel_spacing,
    double& fit_amplitude,
    double& fit_center,
    double& fit_gamma,
    double& fit_beta,
    double& fit_vertical_offset,
    double& fit_amplitude_err,
    double& fit_center_err,
    double& fit_gamma_err,
    double& fit_beta_err,
    double& fit_vertical_offset_err,
    double& chi2_reduced,
    bool verbose,
    bool enable_outlier_filtering) {
    
    if (x_vals.size() != y_vals.size() || x_vals.size() < 5) {
        if (verbose) {
            std::cout << "Insufficient data points for Power Lorentzian fitting" << std::endl;
        }
        return false;
    }
    
    // Multiple outlier filtering strategies
    std::vector<std::pair<std::vector<double>, std::vector<double>>> filtered_datasets;
    
    if (enable_outlier_filtering) {
        auto conservative_data = FilterPowerLorentzianOutliers(x_vals, y_vals, 2.5, verbose);
        if (conservative_data.first.size() >= 5) {
            filtered_datasets.push_back(conservative_data);
        }
        
        auto lenient_data = FilterPowerLorentzianOutliers(x_vals, y_vals, 3.0, verbose);
        if (lenient_data.first.size() >= 5) {
            filtered_datasets.push_back(lenient_data);
        }
    }
    
    // Always include original data as fallback
    filtered_datasets.push_back(std::make_pair(x_vals, y_vals));
    
    if (verbose) {
        std::cout << "Power Lorentzian outlier filtering " << (enable_outlier_filtering ? "enabled" : "disabled") 
                 << ", testing " << filtered_datasets.size() << " datasets" << std::endl;
    }
    
    // Try each filtered dataset
    for (size_t dataset_idx = 0; dataset_idx < filtered_datasets.size(); ++dataset_idx) {
        std::vector<double> clean_x = filtered_datasets[dataset_idx].first;
        std::vector<double> clean_y = filtered_datasets[dataset_idx].second;
        
        if (clean_x.size() < 5) continue;
        
        if (verbose) {
            std::cout << "Trying Power Lorentzian dataset " << dataset_idx << " with " << clean_x.size() << " points" << std::endl;
        }
        
        // Get parameter estimates
        PowerLorentzianParameterEstimates estimates = EstimatePowerLorentzianParameters(clean_x, clean_y, center_estimate, pixel_spacing, verbose);
        if (!estimates.valid) {
            if (verbose) {
                std::cout << "Power Lorentzian parameter estimation failed for dataset " << dataset_idx << std::endl;
            }
            continue;
        }
        
        // Calculate uncertainty as 5% of max charge
        double max_charge = *std::max_element(clean_y.begin(), clean_y.end());
        double uncertainty = CalculatePowerLorentzianUncertainty(max_charge);
        
        // Multiple fitting configurations (similar to Lorentzian)
        struct PowerLorentzianFittingConfig {
            ceres::LinearSolverType linear_solver;
            ceres::TrustRegionStrategyType trust_region;
            double function_tolerance;
            double gradient_tolerance;
            int max_iterations;
            std::string loss_function;
            double loss_parameter;
        };
        
        std::vector<PowerLorentzianFittingConfig> configs;
        
        PowerLorentzianFittingConfig config1;
        config1.linear_solver = ceres::DENSE_QR;
        config1.trust_region = ceres::LEVENBERG_MARQUARDT;
        config1.function_tolerance = 1e-15;
        config1.gradient_tolerance = 1e-15;
        config1.max_iterations = 2000;
        config1.loss_function = "HUBER";
        config1.loss_parameter = estimates.amplitude * 0.1;
        configs.push_back(config1);
        
        PowerLorentzianFittingConfig config2;
        config2.linear_solver = ceres::DENSE_QR;
        config2.trust_region = ceres::LEVENBERG_MARQUARDT;
        config2.function_tolerance = 1e-12;
        config2.gradient_tolerance = 1e-12;
        config2.max_iterations = 1500;
        config2.loss_function = "CAUCHY";
        config2.loss_parameter = estimates.amplitude * 0.16;
        configs.push_back(config2);
        
        PowerLorentzianFittingConfig config3;
        config3.linear_solver = ceres::DENSE_QR;
        config3.trust_region = ceres::DOGLEG;
        config3.function_tolerance = 1e-10;
        config3.gradient_tolerance = 1e-10;
        config3.max_iterations = 1000;
        config3.loss_function = "NONE";
        config3.loss_parameter = 0.0;
        configs.push_back(config3);
        
        PowerLorentzianFittingConfig config4;
        config4.linear_solver = ceres::DENSE_NORMAL_CHOLESKY;
        config4.trust_region = ceres::LEVENBERG_MARQUARDT;
        config4.function_tolerance = 1e-12;
        config4.gradient_tolerance = 1e-12;
        config4.max_iterations = 1500;
        config4.loss_function = "HUBER";
        config4.loss_parameter = estimates.amplitude * 0.13;
        configs.push_back(config4);
        
        PowerLorentzianFittingConfig config5;
        config5.linear_solver = ceres::SPARSE_NORMAL_CHOLESKY;
        config5.trust_region = ceres::LEVENBERG_MARQUARDT;
        config5.function_tolerance = 1e-12;
        config5.gradient_tolerance = 1e-12;
        config5.max_iterations = 1200;
        config5.loss_function = "CAUCHY";
        config5.loss_parameter = estimates.amplitude * 0.22;
        configs.push_back(config5);
        
        for (const auto& config : configs) {
            // Set up parameter array (5 parameters: A, m, gamma, beta, B)
            double parameters[5];
            parameters[0] = estimates.amplitude;
            parameters[1] = estimates.center;
            parameters[2] = estimates.gamma;
            parameters[3] = estimates.beta;
            parameters[4] = estimates.baseline;
            
            // Build the problem
            ceres::Problem problem;
            
            // Add residual blocks with uncertainties
            for (size_t i = 0; i < clean_x.size(); ++i) {
                ceres::CostFunction* cost_function = PowerLorentzianCostFunction::Create(
                    clean_x[i], clean_y[i], uncertainty);
                
                // No loss functions - simple weighted least squares
                problem.AddResidualBlock(cost_function, nullptr, parameters);
            }
            
            // Set bounds (similar to Lorentzian but with extra beta parameter)
            double amp_min = std::max(Constants::MIN_UNCERTAINTY_VALUE, estimates.amplitude * 0.01);
            double max_charge_val = *std::max_element(clean_y.begin(), clean_y.end());
            double physics_amp_max = max_charge_val * 1.5;
            double algo_amp_max = estimates.amplitude * 100.0;
            double amp_max = std::min(physics_amp_max, algo_amp_max);

            problem.SetParameterLowerBound(parameters, 0, amp_min);
            problem.SetParameterUpperBound(parameters, 0, amp_max);
            
            double center_range = pixel_spacing * 3.0;
            problem.SetParameterLowerBound(parameters, 1, estimates.center - center_range);
            problem.SetParameterUpperBound(parameters, 1, estimates.center + center_range);
            
            problem.SetParameterLowerBound(parameters, 2, pixel_spacing * 0.05);
            problem.SetParameterUpperBound(parameters, 2, pixel_spacing * 4.0);
            
            // Beta parameter bounds (power exponent): [0.2, 4.0]
            problem.SetParameterLowerBound(parameters, 3, 0.2);
            problem.SetParameterUpperBound(parameters, 3, 4.0);
            
            double baseline_range = std::max(estimates.amplitude * 0.5, std::abs(estimates.baseline) * 2.0);
            problem.SetParameterLowerBound(parameters, 4, estimates.baseline - baseline_range);
            problem.SetParameterUpperBound(parameters, 4, estimates.baseline + baseline_range);
            
            // Configure solver
            ceres::Solver::Options options;
            options.linear_solver_type = config.linear_solver;
            options.minimizer_type = ceres::TRUST_REGION;
            options.trust_region_strategy_type = config.trust_region;
            options.function_tolerance = config.function_tolerance;
            options.gradient_tolerance = config.gradient_tolerance;
            options.parameter_tolerance = 1e-15;
            options.max_num_iterations = config.max_iterations;
            options.max_num_consecutive_invalid_steps = 50;
            options.use_nonmonotonic_steps = true;
            options.minimizer_progress_to_stdout = false;
            
            // Two-stage fitting approach to ensure consistent center with Lorentzian
            
            // Stage 1: Constrain beta close to 1.0 (standard Lorentzian) to stabilize center
            problem.SetParameterLowerBound(parameters, 3, 0.9);
            problem.SetParameterUpperBound(parameters, 3, 1.1);
            
            if (verbose) {
                std::cout << "Stage 1: Fitting with beta constrained to ~1.0 (Lorentzian-like)..." << std::endl;
            }
            
            ceres::Solver::Summary summary_stage1;
            ceres::Solve(options, &problem, &summary_stage1);
            
            // Check if stage 1 was successful
            bool stage1_successful = (summary_stage1.termination_type == ceres::CONVERGENCE ||
                                    summary_stage1.termination_type == ceres::USER_SUCCESS) &&
                                   parameters[0] > 0 && parameters[2] > 0 &&
                                   !std::isnan(parameters[0]) && !std::isnan(parameters[1]) &&
                                   !std::isnan(parameters[2]) && !std::isnan(parameters[3]) &&
                                   !std::isnan(parameters[4]);
            
            ceres::Solver::Summary summary;
            if (stage1_successful) {
                // Stage 2: Allow beta to vary more freely while keeping the stabilized center
                problem.SetParameterLowerBound(parameters, 3, 0.2);
                problem.SetParameterUpperBound(parameters, 3, 4.0);
                
                // Tighten center bounds around the Stage 1 result to prevent drift
                double stage1_center = parameters[1];
                double tight_center_range = pixel_spacing * 0.5; // Much tighter than original
                problem.SetParameterLowerBound(parameters, 1, stage1_center - tight_center_range);
                problem.SetParameterUpperBound(parameters, 1, stage1_center + tight_center_range);
                
                if (verbose) {
                    std::cout << "Stage 2: Refining fit with beta free to vary (center stabilized at " 
                             << stage1_center << ")..." << std::endl;
                }
                
                ceres::Solve(options, &problem, &summary);
            } else {
                // If stage 1 failed, use the original single-stage approach
                problem.SetParameterLowerBound(parameters, 3, 0.2);
                problem.SetParameterUpperBound(parameters, 3, 4.0);
                
                if (verbose) {
                    std::cout << "Stage 1 failed, falling back to single-stage fit..." << std::endl;
                }
                
                ceres::Solve(options, &problem, &summary);
            }
            
            // Validate results (including beta parameter validation)
            bool fit_successful = (summary.termination_type == ceres::CONVERGENCE ||
                                  summary.termination_type == ceres::USER_SUCCESS) &&
                                 parameters[0] > 0 && parameters[2] > 0 && parameters[3] > 0.1 &&
                                 parameters[3] < 5.0 && // Beta constraint
                                 !std::isnan(parameters[0]) && !std::isnan(parameters[1]) &&
                                 !std::isnan(parameters[2]) && !std::isnan(parameters[3]) &&
                                 !std::isnan(parameters[4]);
            
            if (fit_successful) {
                // Extract results
                fit_amplitude = parameters[0];
                fit_center = parameters[1];
                fit_gamma = std::abs(parameters[2]);
                fit_beta = parameters[3];
                fit_vertical_offset = parameters[4];
                
                // Calculate uncertainties (similar to Lorentzian approach)
                bool cov_success = false;
                
                std::vector<std::pair<ceres::CovarianceAlgorithmType, double>> cov_configs = {
                    {ceres::DENSE_SVD, 1e-14},
                    {ceres::DENSE_SVD, 1e-12},
                    {ceres::DENSE_SVD, 1e-10},
                    {ceres::SPARSE_QR, 1e-12}
                };
                
                for (const auto& cov_config : cov_configs) {
                    ceres::Covariance::Options cov_options;
                    cov_options.algorithm_type = cov_config.first;
                    cov_options.min_reciprocal_condition_number = cov_config.second;
                    cov_options.null_space_rank = 2;
                    cov_options.apply_loss_function = true;
                    
                    ceres::Covariance covariance(cov_options);
                    std::vector<std::pair<const double*, const double*>> covariance_blocks;
                    covariance_blocks.push_back(std::make_pair(parameters, parameters));
                    
                    if (covariance.Compute(covariance_blocks, &problem)) {
                        double covariance_matrix[25]; // 5x5 matrix for 5 parameters
                        if (covariance.GetCovarianceBlock(parameters, parameters, covariance_matrix)) {
                            fit_amplitude_err = std::sqrt(std::abs(covariance_matrix[0]));
                            fit_center_err = std::sqrt(std::abs(covariance_matrix[6]));
                            fit_gamma_err = std::sqrt(std::abs(covariance_matrix[12]));
                            fit_beta_err = std::sqrt(std::abs(covariance_matrix[18]));
                            fit_vertical_offset_err = std::sqrt(std::abs(covariance_matrix[24]));
                            
                            if (!std::isnan(fit_amplitude_err) && !std::isnan(fit_center_err) &&
                                !std::isnan(fit_gamma_err) && !std::isnan(fit_beta_err) && 
                                !std::isnan(fit_vertical_offset_err) &&
                                fit_amplitude_err < 10.0 * fit_amplitude &&
                                fit_center_err < 5.0 * pixel_spacing) {
                                cov_success = true;
                                break;
                            }
                        }
                    }
                }
                
                // Fallback uncertainty estimation
                if (!cov_success) {
                    DataStatistics data_stats = CalculateRobustStatisticsPowerLorentzian(clean_x, clean_y);
                    fit_amplitude_err = std::max(0.02 * fit_amplitude, 0.1 * data_stats.mad);
                    fit_center_err = std::max(0.02 * pixel_spacing, fit_gamma / 10.0);
                    fit_gamma_err = std::max(0.05 * fit_gamma, 0.01 * pixel_spacing);
                    fit_beta_err = std::max(0.1 * fit_beta, 0.05);
                    fit_vertical_offset_err = std::max(0.1 * std::abs(fit_vertical_offset), 0.05 * data_stats.mad);
                }
                
                // Calculate reduced chi-squared
                // Ceres returns 0.5 * Σ r_i^2, multiply by 2 to get χ².
                double chi2 = summary.final_cost * 2.0;
                int dof = std::max(1, static_cast<int>(clean_x.size()) - 5); // DOF for 5 parameters
                chi2_reduced = chi2 / dof;
                
                if (verbose) {
                    std::cout << "Successful Power Lorentzian fit with config " << &config - &configs[0] 
                             << ", dataset " << dataset_idx 
                             << ": A=" << fit_amplitude << "±" << fit_amplitude_err
                             << ", m=" << fit_center << "±" << fit_center_err
                             << ", gamma=" << fit_gamma << "±" << fit_gamma_err
                             << ", beta=" << fit_beta << "±" << fit_beta_err
                             << ", B=" << fit_vertical_offset << "±" << fit_vertical_offset_err
                             << ", chi2red=" << chi2_reduced << std::endl;
                }
                
                return true;
            } else if (verbose) {
                std::cout << "Power Lorentzian fit failed with config " << &config - &configs[0] 
                         << ": " << summary.BriefReport() << std::endl;
            }
        }
    }
    
    if (verbose) {
        std::cout << "All Power Lorentzian fitting strategies failed" << std::endl;
    }
    return false;
}

PowerLorentzianFit2DResultsCeres Fit2DPowerLorentzianCeres(
    const std::vector<double>& x_coords,
    const std::vector<double>& y_coords, 
    const std::vector<double>& charge_values,
    double center_x_estimate,
    double center_y_estimate,
    double pixel_spacing,
    bool verbose,
    bool enable_outlier_filtering)
{
    PowerLorentzianFit2DResultsCeres result;
    
    // Thread-safe Ceres operations
    std::lock_guard<std::mutex> lock(gCeresPowerLorentzianFitMutex);
    
    // Initialize Ceres logging
    InitializeCeresPowerLorentzian();
    
    if (x_coords.size() != y_coords.size() || x_coords.size() != charge_values.size()) {
        if (verbose) {
            std::cout << "Fit2DPowerLorentzianCeres: Error - coordinate and charge vector sizes don't match" << std::endl;
        }
        return result;
    }
    
    if (x_coords.size() < 5) {
        if (verbose) {
            std::cout << "Fit2DPowerLorentzianCeres: Error - need at least 5 data points for fitting" << std::endl;
        }
        return result;
    }
    
    if (verbose) {
        std::cout << "Starting 2D Power Lorentzian fit (Ceres) with " << x_coords.size() << " data points" << std::endl;
    }
    
    // Create maps to group data by rows and columns
    std::map<double, std::vector<std::pair<double, double>>> rows_data; // y -> [(x, charge), ...]
    std::map<double, std::vector<std::pair<double, double>>> cols_data; // x -> [(y, charge), ...]
    
    // Group data points by rows and columns (within pixel spacing tolerance)
    const double tolerance = pixel_spacing * 0.1; // 10% tolerance for grouping
    
    for (size_t i = 0; i < x_coords.size(); ++i) {
        double x = x_coords[i];
        double y = y_coords[i];
        double charge = charge_values[i];
        
        if (charge <= 0) continue; // Skip non-positive charges
        
        // Find or create row
        bool found_row = false;
        for (auto& row_pair : rows_data) {
            if (std::abs(row_pair.first - y) < tolerance) {
                row_pair.second.push_back(std::make_pair(x, charge));
                found_row = true;
                break;
            }
        }
        if (!found_row) {
            rows_data[y].push_back(std::make_pair(x, charge));
        }
        
        // Find or create column
        bool found_col = false;
        for (auto& col_pair : cols_data) {
            if (std::abs(col_pair.first - x) < tolerance) {
                col_pair.second.push_back(std::make_pair(y, charge));
                found_col = true;
                break;
            }
        }
        if (!found_col) {
            cols_data[x].push_back(std::make_pair(y, charge));
        }
    }
    
    // Find the row and column closest to the center estimates
    double best_row_y = center_y_estimate;
    double min_row_dist = std::numeric_limits<double>::max();
    for (const auto& row_pair : rows_data) {
        double dist = std::abs(row_pair.first - center_y_estimate);
        if (dist < min_row_dist && row_pair.second.size() >= 5) {
            min_row_dist = dist;
            best_row_y = row_pair.first;
        }
    }
    
    double best_col_x = center_x_estimate;
    double min_col_dist = std::numeric_limits<double>::max();
    for (const auto& col_pair : cols_data) {
        double dist = std::abs(col_pair.first - center_x_estimate);
        if (dist < min_col_dist && col_pair.second.size() >= 5) {
            min_col_dist = dist;
            best_col_x = col_pair.first;
        }
    }
    
    bool x_fit_success = false;
    bool y_fit_success = false;
    
    // Fit X direction (central row)
    if (rows_data.find(best_row_y) != rows_data.end() && rows_data[best_row_y].size() >= 5) {
        auto& row_data = rows_data[best_row_y];
        
        // Sort by X coordinate
        std::sort(row_data.begin(), row_data.end());
        
        // Create vectors for fitting
        std::vector<double> x_vals, y_vals;
        std::vector<double> row_x_coords, row_y_coords;
        for (const auto& point : row_data) {
            x_vals.push_back(point.first);
            y_vals.push_back(point.second);
            row_x_coords.push_back(point.first);
            row_y_coords.push_back(best_row_y);  // Y coordinate is constant for row
        }
        
        if (verbose) {
            std::cout << "Fitting Power Lorentzian X direction with " << x_vals.size() << " points" << std::endl;
        }
        
        x_fit_success = FitPowerLorentzianCeres(
            x_vals, y_vals, center_x_estimate, pixel_spacing,
            result.x_amplitude, result.x_center, result.x_gamma, result.x_beta, result.x_vertical_offset,
            result.x_amplitude_err, result.x_center_err, result.x_gamma_err, result.x_beta_err, result.x_vertical_offset_err,
            result.x_chi2red, verbose, enable_outlier_filtering);
        
        // Calculate DOF and p-value
        result.x_dof = std::max(1, static_cast<int>(x_vals.size()) - 5); // Corrected DOF for 5 parameters
        result.x_pp = (result.x_chi2red > 0) ? 1.0 - std::min(1.0, result.x_chi2red / 10.0) : 0.0;
        
        // Store data for ROOT analysis
        result.x_row_pixel_coords = x_vals;
        result.x_row_charge_values = y_vals;
        result.x_row_charge_errors = std::vector<double>(); // Empty vector
    }
    
    // Fit Y direction (central column)
    if (cols_data.find(best_col_x) != cols_data.end() && cols_data[best_col_x].size() >= 5) {
        auto& col_data = cols_data[best_col_x];
        
        // Sort by Y coordinate
        std::sort(col_data.begin(), col_data.end());
        
        // Create vectors for fitting
        std::vector<double> x_vals, y_vals;
        std::vector<double> col_x_coords, col_y_coords;
        for (const auto& point : col_data) {
            x_vals.push_back(point.first); // Y coordinate
            y_vals.push_back(point.second); // charge
            col_x_coords.push_back(best_col_x);  // X coordinate is constant for column
            col_y_coords.push_back(point.first); // Y coordinate
        }
        
        if (verbose) {
            std::cout << "Fitting Power Lorentzian Y direction with " << x_vals.size() << " points" << std::endl;
        }
        
        y_fit_success = FitPowerLorentzianCeres(
            x_vals, y_vals, center_y_estimate, pixel_spacing,
            result.y_amplitude, result.y_center, result.y_gamma, result.y_beta, result.y_vertical_offset,
            result.y_amplitude_err, result.y_center_err, result.y_gamma_err, result.y_beta_err, result.y_vertical_offset_err,
            result.y_chi2red, verbose, enable_outlier_filtering);
        
        // Calculate DOF and p-value
        result.y_dof = std::max(1, static_cast<int>(x_vals.size()) - 5); // Corrected DOF for 5 parameters
        result.y_pp = (result.y_chi2red > 0) ? 1.0 - std::min(1.0, result.y_chi2red / 10.0) : 0.0;
        
        // Store data for ROOT analysis
        result.y_col_pixel_coords = x_vals;  // Y coordinates
        result.y_col_charge_values = y_vals;
        result.y_col_charge_errors = std::vector<double>(); // Empty vector
    }
    
    // Set overall success status
    result.fit_successful = x_fit_success && y_fit_success;
    
    // Calculate and store charge uncertainties (5% of max charge for each direction) only if enabled
    if (Constants::ENABLE_VERTICAL_CHARGE_UNCERTAINTIES) {
        if (x_fit_success && rows_data.find(best_row_y) != rows_data.end()) {
            auto& row_data = rows_data[best_row_y];
            double max_charge_x = 0.0;
            for (const auto& point : row_data) {
                max_charge_x = std::max(max_charge_x, point.second);
            }
            result.x_charge_uncertainty = 0.05 * max_charge_x;
        }
        
        if (y_fit_success && cols_data.find(best_col_x) != cols_data.end()) {
            auto& col_data = cols_data[best_col_x];
            double max_charge_y = 0.0;
            for (const auto& point : col_data) {
                max_charge_y = std::max(max_charge_y, point.second);
            }
            result.y_charge_uncertainty = 0.05 * max_charge_y;
        }
    } else {
        result.x_charge_uncertainty = 0.0;
        result.y_charge_uncertainty = 0.0;
    }
    
    if (verbose) {
        std::cout << "2D Power Lorentzian fit (Ceres) " << (result.fit_successful ? "successful" : "failed") 
                 << " (X: " << (x_fit_success ? "OK" : "FAIL") 
                 << ", Y: " << (y_fit_success ? "OK" : "FAIL") << ")" << std::endl;
    }
    
    return result;
}

// Outlier removal function for Power Lorentzian fitting
PowerLorentzianOutlierRemovalResult RemovePowerLorentzianOutliers(
    const std::vector<double>& x_coords,
    const std::vector<double>& y_coords,
    const std::vector<double>& charge_values,
    bool enable_outlier_removal,
    double sigma_threshold,
    bool verbose)
{
    PowerLorentzianOutlierRemovalResult result;
    
    if (x_coords.size() != y_coords.size() || x_coords.size() != charge_values.size()) {
        if (verbose) {
            std::cout << "RemovePowerLorentzianOutliers: Error - coordinate and charge vector sizes don't match" << std::endl;
        }
        return result;
    }
    
    if (!enable_outlier_removal) {
        // No filtering requested - return original data
        result.filtered_x_coords = x_coords;
        result.filtered_y_coords = y_coords;
        result.filtered_charge_values = charge_values;
        result.outliers_removed = 0;
        result.filtering_applied = false;
        result.success = true;
        return result;
    }
    
    if (charge_values.size() < 5) {
        // Not enough data for outlier removal
        result.filtered_x_coords = x_coords;
        result.filtered_y_coords = y_coords;
        result.filtered_charge_values = charge_values;
        result.outliers_removed = 0;
        result.filtering_applied = false;
        result.success = true;
        return result;
    }
    
    // Calculate robust statistics for outlier detection
    DataStatistics stats = CalculateRobustStatisticsPowerLorentzian(x_coords, charge_values);
    if (!stats.valid) {
        // Fall back to original data if statistics calculation fails
        result.filtered_x_coords = x_coords;
        result.filtered_y_coords = y_coords;
        result.filtered_charge_values = charge_values;
        result.outliers_removed = 0;
        result.filtering_applied = false;
        result.success = false;
        return result;
    }
    
    // Identify outliers using median absolute deviation
    double outlier_threshold = sigma_threshold * stats.mad;
    std::vector<bool> is_outlier(charge_values.size(), false);
    int outlier_count = 0;
    
    for (size_t i = 0; i < charge_values.size(); ++i) {
        double deviation = std::abs(charge_values[i] - stats.median);
        if (deviation > outlier_threshold) {
            is_outlier[i] = true;
            outlier_count++;
        }
    }
    
    // Ensure we don't remove too many points (keep at least 5)
    if (charge_values.size() - outlier_count < 5) {
        // Too many outliers detected - reduce threshold or keep original data
        result.filtered_x_coords = x_coords;
        result.filtered_y_coords = y_coords;
        result.filtered_charge_values = charge_values;
        result.outliers_removed = 0;
        result.filtering_applied = false;
        result.success = true;
        
        if (verbose) {
            std::cout << "RemovePowerLorentzianOutliers: Too many outliers detected (" << outlier_count 
                     << "), keeping original data" << std::endl;
        }
        return result;
    }
    
    // Filter out outliers
    for (size_t i = 0; i < charge_values.size(); ++i) {
        if (!is_outlier[i]) {
            result.filtered_x_coords.push_back(x_coords[i]);
            result.filtered_y_coords.push_back(y_coords[i]);
            result.filtered_charge_values.push_back(charge_values[i]);
        }
    }
    
    result.outliers_removed = outlier_count;
    result.filtering_applied = true;
    result.success = true;
    
    if (verbose) {
        std::cout << "RemovePowerLorentzianOutliers: Removed " << outlier_count 
                 << " outliers, " << result.filtered_charge_values.size() << " points remaining" << std::endl;
    }
    
    return result;
}

// Diagonal Power Lorentzian fitting function
DiagonalPowerLorentzianFitResultsCeres FitDiagonalPowerLorentzianCeres(
    const std::vector<double>& x_coords,
    const std::vector<double>& y_coords, 
    const std::vector<double>& charge_values,
    double center_x_estimate,
    double center_y_estimate,
    double pixel_spacing,
    bool verbose,
    bool enable_outlier_filtering)
{
    DiagonalPowerLorentzianFitResultsCeres result;
    
    // Thread-safe Ceres operations
    std::lock_guard<std::mutex> lock(gCeresPowerLorentzianFitMutex);
    
    // Initialize Ceres logging
    InitializeCeresPowerLorentzian();
    
    if (x_coords.size() != y_coords.size() || x_coords.size() != charge_values.size()) {
        if (verbose) {
            std::cout << "FitDiagonalPowerLorentzianCeres: Error - coordinate and charge vector sizes don't match" << std::endl;
        }
        return result;
    }
    
    if (x_coords.size() < 5) {
        if (verbose) {
            std::cout << "FitDiagonalPowerLorentzianCeres: Error - need at least 5 data points for fitting" << std::endl;
        }
        return result;
    }
    
    if (verbose) {
        std::cout << "Starting diagonal Power Lorentzian fit (Ceres) with " << x_coords.size() << " data points" << std::endl;
    }
    
    // Apply outlier filtering if requested
    std::vector<double> filtered_x_coords = x_coords;
    std::vector<double> filtered_y_coords = y_coords;
    std::vector<double> filtered_charge_values = charge_values;
    
    if (enable_outlier_filtering) {
        PowerLorentzianOutlierRemovalResult filter_result = RemovePowerLorentzianOutliers(
            x_coords, y_coords, charge_values, true, 2.5, verbose);
        
        if (filter_result.success && filter_result.filtering_applied) {
            filtered_x_coords = filter_result.filtered_x_coords;
            filtered_y_coords = filter_result.filtered_y_coords;
            filtered_charge_values = filter_result.filtered_charge_values;
        }
    }
    
    // Effective centre-to-centre pitch along diagonal
    const double diag_pixel_spacing = pixel_spacing * 1.41421356237; // √2 × pitch

    // Create diagonal data arrays
    // Main diagonal: points where (x-center_x) ≈ (y-center_y)
    // Secondary diagonal: points where (x-center_x) ≈ -(y-center_y)
    std::vector<std::pair<double, double>> main_diag_x_data, main_diag_y_data;
    std::vector<std::pair<double, double>> sec_diag_x_data, sec_diag_y_data;
    
    const double diagonal_tolerance = pixel_spacing * 0.5; // Tolerance for diagonal selection
    
    for (size_t i = 0; i < filtered_x_coords.size(); ++i) {
        double x = filtered_x_coords[i];
        double y = filtered_y_coords[i];
        double charge = filtered_charge_values[i];
        
        if (charge <= 0) continue;
        
        double dx = x - center_x_estimate;
        double dy = y - center_y_estimate;
        
        // Check if point is on main diagonal (dx ≈ dy)
        if (std::abs(dx - dy) < diagonal_tolerance) {
            double diag_coord = (dx + dy) / 2.0; // Average coordinate along diagonal
            main_diag_x_data.push_back(std::make_pair(diag_coord, charge));
            main_diag_y_data.push_back(std::make_pair(diag_coord, charge));
        }
        
        // Check if point is on secondary diagonal (dx ≈ -dy)
        if (std::abs(dx + dy) < diagonal_tolerance) {
            double diag_coord = (dx - dy) / 2.0; // Coordinate along secondary diagonal
            sec_diag_x_data.push_back(std::make_pair(diag_coord, charge));
            sec_diag_y_data.push_back(std::make_pair(diag_coord, charge));
        }
    }
    
    // Fit main diagonal X direction
    if (main_diag_x_data.size() >= 5) {
        std::sort(main_diag_x_data.begin(), main_diag_x_data.end());
        
        std::vector<double> x_vals, y_vals;
        for (const auto& point : main_diag_x_data) {
            x_vals.push_back(point.first);
            y_vals.push_back(point.second);
        }
        
        if (verbose) {
            std::cout << "Fitting main diagonal X with " << x_vals.size() << " points" << std::endl;
        }
        
        result.main_diag_x_fit_successful = FitPowerLorentzianCeres(
            x_vals, y_vals, 0.0, diag_pixel_spacing,
            result.main_diag_x_amplitude, result.main_diag_x_center, result.main_diag_x_gamma, 
            result.main_diag_x_beta, result.main_diag_x_vertical_offset,
            result.main_diag_x_amplitude_err, result.main_diag_x_center_err, result.main_diag_x_gamma_err,
            result.main_diag_x_beta_err, result.main_diag_x_vertical_offset_err,
            result.main_diag_x_chi2red, verbose, enable_outlier_filtering);
        
        result.main_diag_x_dof = std::max(1, static_cast<int>(x_vals.size()) - 5);
        result.main_diag_x_pp = (result.main_diag_x_chi2red > 0) ? 1.0 - std::min(1.0, result.main_diag_x_chi2red / 10.0) : 0.0;
    }
    
    // Fit main diagonal Y direction (same data as X)
    if (main_diag_y_data.size() >= 5) {
        std::sort(main_diag_y_data.begin(), main_diag_y_data.end());
        
        std::vector<double> x_vals, y_vals;
        for (const auto& point : main_diag_y_data) {
            x_vals.push_back(point.first);
            y_vals.push_back(point.second);
        }
        
        if (verbose) {
            std::cout << "Fitting main diagonal Y with " << x_vals.size() << " points" << std::endl;
        }
        
        result.main_diag_y_fit_successful = FitPowerLorentzianCeres(
            x_vals, y_vals, 0.0, diag_pixel_spacing,
            result.main_diag_y_amplitude, result.main_diag_y_center, result.main_diag_y_gamma,
            result.main_diag_y_beta, result.main_diag_y_vertical_offset,
            result.main_diag_y_amplitude_err, result.main_diag_y_center_err, result.main_diag_y_gamma_err,
            result.main_diag_y_beta_err, result.main_diag_y_vertical_offset_err,
            result.main_diag_y_chi2red, verbose, enable_outlier_filtering);
        
        result.main_diag_y_dof = std::max(1, static_cast<int>(x_vals.size()) - 5);
        result.main_diag_y_pp = (result.main_diag_y_chi2red > 0) ? 1.0 - std::min(1.0, result.main_diag_y_chi2red / 10.0) : 0.0;
    }
    
    // Fit secondary diagonal X direction
    if (sec_diag_x_data.size() >= 5) {
        std::sort(sec_diag_x_data.begin(), sec_diag_x_data.end());
        
        std::vector<double> x_vals, y_vals;
        for (const auto& point : sec_diag_x_data) {
            x_vals.push_back(point.first);
            y_vals.push_back(point.second);
        }
        
        if (verbose) {
            std::cout << "Fitting secondary diagonal X with " << x_vals.size() << " points" << std::endl;
        }
        
        result.sec_diag_x_fit_successful = FitPowerLorentzianCeres(
            x_vals, y_vals, 0.0, diag_pixel_spacing,
            result.sec_diag_x_amplitude, result.sec_diag_x_center, result.sec_diag_x_gamma,
            result.sec_diag_x_beta, result.sec_diag_x_vertical_offset,
            result.sec_diag_x_amplitude_err, result.sec_diag_x_center_err, result.sec_diag_x_gamma_err,
            result.sec_diag_x_beta_err, result.sec_diag_x_vertical_offset_err,
            result.sec_diag_x_chi2red, verbose, enable_outlier_filtering);
        
        result.sec_diag_x_dof = std::max(1, static_cast<int>(x_vals.size()) - 5);
        result.sec_diag_x_pp = (result.sec_diag_x_chi2red > 0) ? 1.0 - std::min(1.0, result.sec_diag_x_chi2red / 10.0) : 0.0;
    }
    
    // Fit secondary diagonal Y direction
    if (sec_diag_y_data.size() >= 5) {
        std::sort(sec_diag_y_data.begin(), sec_diag_y_data.end());
        
        std::vector<double> x_vals, y_vals;
        for (const auto& point : sec_diag_y_data) {
            x_vals.push_back(point.first);
            y_vals.push_back(point.second);
        }
        
        if (verbose) {
            std::cout << "Fitting secondary diagonal Y with " << x_vals.size() << " points" << std::endl;
        }
        
        result.sec_diag_y_fit_successful = FitPowerLorentzianCeres(
            x_vals, y_vals, 0.0, diag_pixel_spacing,
            result.sec_diag_y_amplitude, result.sec_diag_y_center, result.sec_diag_y_gamma,
            result.sec_diag_y_beta, result.sec_diag_y_vertical_offset,
            result.sec_diag_y_amplitude_err, result.sec_diag_y_center_err, result.sec_diag_y_gamma_err,
            result.sec_diag_y_beta_err, result.sec_diag_y_vertical_offset_err,
            result.sec_diag_y_chi2red, verbose, enable_outlier_filtering);
        
        result.sec_diag_y_dof = std::max(1, static_cast<int>(x_vals.size()) - 5);
        result.sec_diag_y_pp = (result.sec_diag_y_chi2red > 0) ? 1.0 - std::min(1.0, result.sec_diag_y_chi2red / 10.0) : 0.0;
    }
    
    // Set overall success status
    result.fit_successful = result.main_diag_x_fit_successful && result.main_diag_y_fit_successful &&
                           result.sec_diag_x_fit_successful && result.sec_diag_y_fit_successful;
    
    if (verbose) {
        std::cout << "Diagonal Power Lorentzian fit (Ceres) " << (result.fit_successful ? "successful" : "partial/failed") 
                 << " (Main X: " << (result.main_diag_x_fit_successful ? "OK" : "FAIL")
                 << ", Main Y: " << (result.main_diag_y_fit_successful ? "OK" : "FAIL")
                 << ", Sec X: " << (result.sec_diag_x_fit_successful ? "OK" : "FAIL")
                 << ", Sec Y: " << (result.sec_diag_y_fit_successful ? "OK" : "FAIL") << ")" << std::endl;
    }
    
    return result;
}

 