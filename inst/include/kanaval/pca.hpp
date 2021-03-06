#ifndef KANAVAL_PCA_HPP
#define KANAVAL_PCA_HPP

#include "H5Cpp.h"
#include <vector>
#include "utils.hpp"
#include "misc.hpp"

/**
 * @file pca.hpp
 *
 * @brief Validate PCA contents.
 */

namespace kanaval {

namespace pca {

/**
 * @cond
 */
inline std::pair<int, std::string> validate_parameters(const H5::Group& handle, int version) {
    auto phandle = utils::check_and_open_group(handle, "parameters");

    auto nhvgs = utils::load_integer_scalar<>(phandle, "num_hvgs");
    if (nhvgs <= 0) {
        throw std::runtime_error("number of HVGs must be positive in 'num_hvgs'");
    }

    auto npcs = utils::load_integer_scalar<>(phandle, "num_pcs");
    if (npcs <= 0) {
        throw std::runtime_error("number of PCs must be positive in 'num_pcs'");
    }

    std::string method;
    if (version >= 1001000) {
        method = utils::load_string(phandle, "block_method");
        check_block_method(method, version);
    }

    return std::make_pair(npcs, method);
}

inline int validate_results(const H5::Group& handle, int max_pcs, std::string block_method, int num_cells, int version) {
    auto rhandle = utils::check_and_open_group(handle, "results");

    int obs_pcs = check_pca_contents(rhandle, max_pcs, num_cells);

    if (version >= 1001000 && version < 2000000) {
        if (block_method == "mnn") {
            utils::check_and_open_dataset(rhandle, "corrected", H5T_FLOAT, { static_cast<size_t>(num_cells), static_cast<size_t>(obs_pcs) });
        }
    }

    return obs_pcs;
}
/**
 * @endcond
 */

/**
 * Check contents for the PCA step on the RNA log-expression matrix.
 * Contents are stored inside an `pca` HDF5 group at the root of the file.
 * The `pca` group itself contains the `parameters` and `results` subgroups.
 *
 * <HR>
 * `parameters` should contain:
 *
 * - `num_hvgs`: a scalar integer containing the number of highly variable genes to use to compute the PCA.
 * - `num_pcs`: a scalar integer containing the number of PCs to compute.
 * - \v1_1{\[**since version 1.1**\] `block_method`: a scalar string specifying the method to use when dealing with multiple blocks in the dataset.
 *   This may be `"none"`, `"regress"` or `"mnn"`.}
 *
 * <HR>
 * `results` should contain:
 *
 * - `pcs`: a 2-dimensional float dataset containing the PC coordinates in a row-major layout.
 *   Each row corresponds to a cell (after QC filtering) and each column corresponds to a PC.
 *   Note that this is deliberately transposed from the Javascript/Wasm representation for easier storage.
 *   \v1_1{\[**since version 1.1**\] If `block_type = "mnn"`, the PCs will be computed using a weighted method that adjusts for differences in the number of cells across blocks.
 *   If `block_type = "regress"`, the PCs will be computed on the residuals after regressing out the block-wise effects.}
 * - `var_exp`: a float dataset of length equal to the number of PCs, containing the percentage of variance explained by each PC.
 *
 * \v1_1{\[**since version 1.1**\] If `block_type = "mnn"`, the `results` group will also contain:}
 *
 * - \v1_1{`corrected`, a float dataset with the same dimensions as `pcs`, containing the MNN-corrected PCs for each cell.}
 *
 * <HR>
 * @param handle An open HDF5 file handle.
 * @param num_cells Number of cells in the dataset after any quality filtering is applied.
 * @param version Version of the state file.
 *
 * @return The number of computed PCs.
 * If the format is invalid, an error is raised instead.
 */
inline int validate(const H5::H5File& handle, int num_cells, int version = 1001000) {
    auto phandle = utils::check_and_open_group(handle, "pca");

    int npcs;
    std::string bmethod;
    try {
        auto pout = validate_parameters(phandle, version);
        npcs = pout.first;
        bmethod = pout.second;
    } catch (std::exception& e) {
        throw utils::combine_errors(e, "failed to retrieve parameters from 'pca'");
    }

    int obs_pcs;
    try {
        obs_pcs = validate_results(phandle, npcs, bmethod, num_cells, version);
    } catch (std::exception& e) {
        throw utils::combine_errors(e, "failed to retrieve results from 'pca'");
    }

    return obs_pcs;
}

}

}

#endif
