#ifndef KANAVAL_COMBINE_EMBEDDINGS_HPP
#define KANAVAL_COMBINE_EMBEDDINGS_HPP

#include "H5Cpp.h"
#include <vector>
#include <numeric>
#include "utils.hpp"

/**
 * @file combine_embeddings.hpp
 *
 * @brief Validate combined embedding contents.
 */

namespace kanaval {

namespace combine_embeddings {

/**
 * @cond
 */
inline void validate_parameters(const H5::Group& handle, const std::vector<std::string>& modalities) {
    auto phandle = utils::check_and_open_group(handle, "parameters");
    utils::check_and_open_scalar(phandle, "approximate", H5T_INTEGER);

    auto whandle = utils::check_and_open_group(phandle, "weights");
    if (whandle.getNumObjs()) { // if non-zero weights, all modalities should be present.
        for (const auto& m : modalities) {
            utils::check_and_open_scalar(whandle, m, H5T_FLOAT);
        }
    }
    
    return;
}

inline void validate_results(const H5::Group& handle, int num_cells, const std::vector<std::string>& modalities, int total_dims) {
    auto rhandle = utils::check_and_open_group(handle, "results");

    if (modalities.size() > 1) {
        std::vector<size_t> pdims { static_cast<size_t>(num_cells), static_cast<size_t>(total_dims) };
        utils::check_and_open_dataset(rhandle, "combined", H5T_FLOAT, pdims);
    }

    return;
}
/**
 * @endcond
 */

/**
 * Check contents for the combined embeddings step.
 * Contents are stored inside a `combined_embedding` HDF5 group at the root of the file.
 * The `combined_embedding` group itself contains the `parameters` and `results` subgroups.
 *
 * Only a single embedding was generated prior to version 2.0 of the format, so the `combined_embeddings` group may be absent in pre-v2.0 files.
 * No concept of multi-modality exists in earlier versions so downstream steps should use the PCs directly from `pca::validate()`.
 *
 * <HR>
 * `parameters` should contain:
 *
 * - `approximate`: an integer scalar to be interpreted as a boolean,
 *   indicating whether an approximate neighbor search was used to compute the per-embedding scaling factors.
 * - `weights`: a group containing the (scaling) weights to apply to each modality.
 *   If empty, weights are implicitly assumed to be equal to unity for all modalities.
 *   Otherwise, the group should contain a float scalar dataset named after each modality in `modalities`, containing the weight to be applied to each modality.
 *
 * <HR>
 * If `modalities.size() > 1`, `results` should contain:
 *
 * - `combined`: a 2-dimensional float dataset containing the combined embeddings in a row-major layout.
 *   Each row corresponds to a cell (`num_cells`) and each column corresponds to a dimension (`total_dims`).
 *
 * Otherwise, `combined` may be missing, in which case it is assumed that only a single embedding exists in the analysis and no combining is necessary.
 * Downstream steps should instead use the `pcs` of the PCA group of the available modality, see `pca::validate()` or `adt_pca::validate()`.
 *
 * <HR>
 * @param handle An open HDF5 file handle.
 * @param num_cells Number of cells in the dataset after any quality filtering is applied.
 * @param modalities Vector of strings containing the names of the modalities to be combined.
 * Currently, this may be any combination of `"RNA"` or `"ADT"`.
 * @param total_dims Total number of PCs across all modalities in `modalities`. 
 * @param version Version of the state file.
 *
 * @return If the format is invalid, an error is raised.
 */
inline void validate(const H5::H5File& handle, int num_cells, const std::vector<std::string>& modalities, int total_dims, int version) {
    if (version < 2000000) {
        return;
    }

    auto phandle = utils::check_and_open_group(handle, "combine_embeddings");

    try {
        validate_parameters(phandle, modalities);
    } catch (std::exception& e) {
        throw utils::combine_errors(e, "failed to retrieve parameters from 'combine_embeddings'");
    }

    try {
        validate_results(phandle, num_cells, modalities, total_dims);
    } catch (std::exception& e) {
        throw utils::combine_errors(e, "failed to retrieve results from 'combine_embeddings'");
    }

    return;
}

}

}

#endif
