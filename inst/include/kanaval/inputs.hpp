#ifndef KANAVAL_INPUTS_HPP
#define KANAVAL_INPUTS_HPP

#include "H5Cpp.h"
#include "utils.hpp"
#include <stdexcept>
#include <vector>
#include <string>
#include <numeric>
#include <unordered_set>
#include <unordered_map>

/**
 * @file inputs.hpp
 *
 * @brief Validate input contents.
 */

namespace kanaval {

namespace inputs {

/**
 * @brief Details about the dataset.
 */
struct Details {
    /**
     * Available modalities in the dataset.
     * Currently, this may contain `"RNA"` and/or `"ADT"`.
     */
    std::vector<std::string> modalities;

    /**
     * Number of features for each modality listed in `modalities`.
     * For datasets containing multiple samples, each entry contains the number of features in the intersection of feature spaces across all samples.
     */
    std::vector<int> num_features;

    /**
     * Number of cells.
     * For multi-sample datasets, this considers the total number of cells in all samples.
     */
    int num_cells;

    /**
     * Number of samples.
     * Note that a single matrix may contain multiple samples.
     */
    int num_samples;
};

/**
 * @cond
 */
struct ParamDump {
    int num_matrices;
    bool multi_matrix;
    bool multi_sample;
};

inline ParamDump validate_parameters(const H5::Group& handle, bool embedded, int version) {
    auto phandle = utils::check_and_open_group(handle, "parameters");
    ParamDump output;

    // Formats can either be a scalar... or not.
    std::vector<std::string> formats;
    output.multi_matrix = false;
    {
        auto fhandle = utils::check_and_open_dataset(phandle, "format", H5T_STRING);
        auto fspace = fhandle.getSpace();
        if (fspace.getSimpleExtentNdims() == 0) {
            formats.push_back(utils::load_string(fhandle));
        } else {
            if (version < 1001000) {
                throw std::runtime_error("'format' should be a scalar string in version 1.0");
            }
            output.multi_matrix = true;
            formats = utils::load_string_vector(fhandle);
        }
    }
    output.num_matrices = formats.size();

    auto fihandle = utils::check_and_open_group(phandle, "files");
    auto nfiles = fihandle.getNumObjs();

    // Checking the runs.
    std::vector<int> runs;
    if (output.multi_matrix) {
        runs = utils::load_integer_vector(phandle, "sample_groups");
        if (runs.size() != formats.size()) {
            throw std::runtime_error("'sample_groups' and 'format' should have the same length");
        }

        int total_files = std::accumulate(runs.begin(), runs.end(), 0);
        if (total_files != static_cast<int>(nfiles)) {
            throw std::runtime_error("sum of 'sample_groups' is not equal to the length of 'files'");
        }

        // Checking that everyone has unique groups.
        auto names = utils::load_string_vector(phandle, "sample_names");
        if (names.size() != formats.size()) {
            throw std::runtime_error("'sample_names' and 'format' should have the same length");
        }

        std::unordered_set<std::string> stuff;
        for (auto s : names) {
            if (stuff.find(s) != stuff.end()) {
                throw std::runtime_error("duplicated sample name '" + s + "' in 'sample_names'");
            }
            stuff.insert(s);
        }
    } else {
        runs.push_back(nfiles);
    }

    // Checking the files.
    int sofar = 0;
    std::vector<std::pair<hsize_t, hsize_t> > bytes;
    for (size_t r = 0; r < runs.size(); ++r) {
        auto curf = formats[r];
        std::vector<std::string> types;

        for (int s = 0; s < runs[r]; ++s, ++sofar) {
            std::string current = std::to_string(sofar);
            try {
                auto curfihandle = utils::check_and_open_group(fihandle, current);

                utils::check_and_open_dataset(curfihandle, "name", H5T_STRING, {});
                types.push_back(utils::load_string(curfihandle, "type"));

                if (embedded) {
                    bytes.emplace_back(
                        utils::load_integer_scalar<hsize_t>(curfihandle, "offset"),
                        utils::load_integer_scalar<hsize_t>(curfihandle, "size")
                    );
                } else {
                    utils::check_and_open_dataset(curfihandle, "id", H5T_STRING, {});
                }
            } catch (std::exception& e) {
                throw utils::combine_errors(e, "failed to retrieve information for file " + current);
            }
        }

        if (curf == "MatrixMarket") {
            std::unordered_map<std::string, int> expected;
            expected["mtx"] = 0;
            expected["genes"] = 0;
            expected["annotations"] = 0;

            for (auto t : types) {
                auto it = expected.find(t);
                if (it == expected.end()) {
                    throw std::runtime_error("unknown file type '" + t + "' when format is 'MatrixMarket'");
                }
                ++(it->second);
            }

            if (expected["mtx"] != 1) {
                throw std::runtime_error("expected exactly one 'mtx' file when format is 'MatrixMarket'");
            }
            if (expected["genes"] > 1) {
                throw std::runtime_error("expected no more than one 'genes' file when format is 'MatrixMarket'");
            }
            if (expected["annotations"] > 1) {
                throw std::runtime_error("expected no more than one 'annotation' file when format is 'MatrixMarket'");
            }
            
        } else if (curf == "10X") {
            if (types.size() != 1 || types.front() != "h5") {
                throw std::runtime_error("expected exactly one 'h5' file when format is '10X'");
            }
        } else if (curf == "H5AD") {
            if (types.size() != 1 || types.front() != "h5") {
                throw std::runtime_error("expected exactly one 'h5' file when format is 'H5AD'");
            }
        }
    }

    // Checking the files make sense.
    if (embedded) {
        hsize_t sofar = 0;
        for (const auto& b : bytes) {
            if (b.first != sofar) {
                throw std::runtime_error("offsets and sizes of 'files' are not sorted and contiguous");
            }
            sofar += b.second;
        }
    }

    // Checking if there's a batch variable.
    if (!output.multi_matrix && phandle.exists("sample_factor")) {
        utils::check_and_open_dataset(phandle, "sample_factor", H5T_STRING, {});
        output.multi_sample = true;
    }  else {
        output.multi_sample = output.multi_matrix;
    }

    return output;
}

inline Details validate_results(const H5::Group& handle, const ParamDump& param_info, int version) {
    auto rhandle = utils::check_and_open_group(handle, "results");
    Details output;

    // Pulling out the dimensions and modalities.
    if (version < 2000000) {
        output.modalities.push_back("RNA");

        auto dims = utils::load_integer_vector<int>(rhandle, "dimensions");
        if (dims.size() != 2) {
            throw std::runtime_error("'dimensions' should be a dataset of length 2");
        }
        if (dims[0] < 0 || dims[1] < 0) {
            throw std::runtime_error("'dimensions' should contain non-negative integers");
        }
        output.num_features.push_back(dims[0]);
        output.num_cells = dims[1];
    } else {
        output.num_cells = utils::load_integer_scalar<>(rhandle, "num_cells");

        auto fhandle = utils::check_and_open_group(rhandle, "num_features");
        size_t nmodals = fhandle.getNumObjs();
        if (nmodals == 0) {
            throw std::runtime_error("number of modalities should be positive");
        }

        for (hsize_t idx = 0; idx < nmodals; ++idx) {
            std::string modality = fhandle.getObjnameByIdx(idx);
            output.modalities.push_back(modality);
            output.num_features.push_back(utils::load_integer_scalar<>(fhandle, modality));
        }
    }

    // Checking the number of samples.
    output.num_samples = 1;
    if (rhandle.exists("num_samples")) {
        output.num_samples = utils::load_integer_scalar<int>(rhandle, "num_samples");
    }
    if (param_info.multi_matrix) {
        if (output.num_samples != param_info.num_matrices) {
            throw std::runtime_error("'num_samples' should be equal to the number of matrices");
        }
    } else {
        if (!param_info.multi_sample && output.num_samples != 1) {
            throw std::runtime_error("'num_samples' should be 1 for single matrix inputs without 'sample_factor'");
        }
    }

    auto check_unique = [](const std::vector<int>& idx, const std::string& msg) -> void {
        std::unordered_set<int> used;
        used.reserve(idx.size());
        for (auto i : idx) {
            if (i < 0) {
                throw std::runtime_error(msg + " contains negative values");
            } else if (used.find(i) != used.end()) {
                throw std::runtime_error(msg + " contains duplicate values");
            }
            used.insert(i);
        }
    };

    if (version >= 2000000) {
        auto ihandle = utils::check_and_open_group<>(rhandle, "identities");
        for (size_t m = 0; m < output.modalities.size(); ++m) {
            auto idx = utils::load_integer_vector<int>(ihandle, output.modalities[m]);
            if (idx.size() != static_cast<size_t>(output.num_features[m])) {
                throw std::runtime_error("'identities' for modality '" + output.modalities[m] + "' should have length equal to its number of features");
            }
            check_unique(idx, "'identities' for modality '" + output.modalities[m] + "'");
        }

    } else if (version >= 1002000) {
        auto idx = utils::load_integer_vector<int>(rhandle, "identities");
        if (idx.size() != static_cast<size_t>(output.num_features[0])) {
            throw std::runtime_error("'identities' should have length equal to the number of genes");
        }
        check_unique(idx, "'identities'");
        
    } else {
        if (param_info.multi_matrix) {
            auto idx = utils::load_integer_vector<int>(rhandle, "indices");
            if (idx.size() != static_cast<size_t>(output.num_features[0])) {
                throw std::runtime_error("'indices' should have length equal to the number of genes");
            }
            check_unique(idx, "'indices'");

        } else {
            auto perms = utils::load_integer_vector<int>(rhandle, "permutation");
            if (perms.size() != static_cast<size_t>(output.num_features[0])) {
                throw std::runtime_error("'permutation' should have length equal to the number of genes");
            }

            // Note that the code below implies that all consecutive entries are present,
            // otherwise we would see duplicates.
            std::vector<unsigned char> used(perms.size());
            for (auto p : perms) {
                if (p < 0 || static_cast<size_t>(p) >= perms.size()) {
                    throw std::runtime_error("'permutation' contains out-of-range values");
                } else if (used[p]) {
                    throw std::runtime_error("duplicated index in 'permutation'");
                }
                used[p] = 1;
            }
        }
    }

    return output;
}
/**
 * @endcond
 */

/**
 * Check contents for the input step.
 * Contents are stored inside an `inputs` HDF5 group at the root of the file.
 * The `inputs` group itself contains the `parameters` and `results` subgroups.
 *
 * In this section, a "matrix" refers to one or more files describing a single (count) matrix.
 * This should be exactly one file for HDF5-based formats, or multiple files for MatrixMarket formats, e.g., to include feature information - see below for details.
 * Multiple matrices may be supplied, in which case each matrix is assumed to contain data for one sample.
 * Alternatively, a single matrix may contain data for one or more samples as partitioned by a sample factor.
 * 
 * The loaded dataset refers to the in-memory representation of the matrix (for single matrix inputs) or the combined matrices (for multiple inputs).
 * The identities of the rows of the loaded dataset may be a permutation or subset of the rows in the input matrices.
 * This is especially true for multiple inputs where the loaded dataset only contains the intersection of features across inputs.
 *
 * <HR>
 * `parameters` should contain:
 * 
 * - `format`: a scalar string specifying the file format for a single matrix.
 *   This is usually either `"MatrixMarket"`, for a MatrixMarket file with possible feature/barcode annotations;
 *   `"10X"`, for the 10X Genomics HDF5 matrix format;
 *   or `"H5AD"`, for the H5AD format.
 *   Other values are allowed but their interpretation is implementation-defined (e.g., for custom resources). 
 *   For multiple matrices, `format` should instead be a 1-dimensional string dataset of length equal to the number of uploads.
 *   Each element of the dataset is usually one of `"MatrixMarket"`, `"10X"` or `"H5AD"`; 
 *   different values can be present for mixed input formats.
 * - `files`: a group of groups representing an array of input file information.
 *   Each inner group is named by their positional index in the array and contains information about a file in an upload.
 *   Each inner group should contain:
 *   - `type`: a scalar string specifying the type of the file.
 *     For multiple matrices, the constraints below apply to all files corresponding to a single matrix.
 *     - If `format = "MatrixMarket"`, there should be exactly one `type = "mtx"` corresponding to the (possibly Gzipped) `*.mtx` file.
 *       There may be zero or one `type = "genes"`, containing a (possibly Gzipped) TSV file with the Ensembl and gene symbols for each row.
 *       There may be zero or one `type = "annotations"`, containing a (possibly Gzipped) TSV file with the annotations for each column.
 *     - If `format = "10X"` or `"H5AD"`, there should be exactly one `type = "h5"`.
 *     - For other `format`s, any `type` can be used, typically for custom resources.
 *   - `name`: a scalar string specifying the file name as it was provided to **kana**.
 *
 *   If `embedded = true`, we additionally expect:
 *   - `offset`: a scalar integer specifying where the file starts as an offset from the start of the remaining bytes section.
 *     The offset for the first file should be zero, and entries in `files` should be ordered by increasing `offset`.
 *   - `size`: a non-negative scalar integer specifying the number of bytes in the file.
 *     The offset of each file should be equal to the sum of `size` and `offset` for the preceding file.
 *
 *   If `embedded = false`, we expect:
 *   - `id`: a scalar string containing some unique identifier for this file.
 *     The interpretation of `id` is application-specific but usually refers to some cache or database.
 *
 * For multiple matrices, `parameters` should also contain:
 *
 * - `sample_groups`: an integer dataset of length equal to the number of samples.
 *   Each entry specifies the number of files in `files` that belong to a sample.
 *   (All files from the same sample are assumed to be contiguous in the array represented by `files`;
 *   so a `sample_groups` of `[3, 2, 1]` would mean that the first three files belong to the first sample, 
 *   the next 2 files belong to the second sample, and the last file belongs to the third sample.)
 * - `sample_names`: a string dataset of length equal to the number of samples, containing the sample name.
 *
 * For single matrix inputs, `parameters` may also contain:
 *
 * - `sample_factor`: a string scalar specifying the field in the per-cell annotation that contains the sample blocking factor. 
 *   If present, it is assumed that the matrix contains data for multiple samples.
 *
 * <DIV style="color:blue">
 * <details>
 * <summary>For version 1.0</summary>
 * `parameters` should contain:
 * 
 * - `format`: a scalar string specifying the file format for a single matrix.
 *   This is usually either `"MatrixMarket"`, for a MatrixMarket file with possible feature/barcode annotations;
 *   `"10X"`, for the 10X Genomics HDF5 matrix format;
 *   or `"H5AD"`, for the H5AD format.
 *   Other values are allowed but their interpretation is implementation-defined (e.g., for custom resources). 
 *   For multiple matrices, `format` should instead be a 1-dimensional string dataset of length equal to the number of uploads.
 *   Each element of the dataset is usually one of `"MatrixMarket"`, `"10X"` or `"H5AD"`; 
 *   different values can be present for mixed input formats.}
 * - `files`: a group of groups representing an array of input file information.
 *   Each inner group is named by their positional index in the array and contains information about a file in an upload.
 *   Each inner group should contain:
 *   - `type`: a scalar string specifying the type of the file.
 *     - If `format = "MatrixMarket"`, there should be exactly one `type = "mtx"` corresponding to the (possibly Gzipped) `*.mtx` file.
 *       There may be zero or one `type = "genes"`, containing a (possibly Gzipped) TSV file with the Ensembl and gene symbols for each row.
 *       There may be zero or one `type = "annotations"`, containing a (possibly Gzipped) TSV file with the annotations for each column.
 *     - If `format = "10X"` or `"H5AD"`, there should be exactly one `type = "h5"`.
 *     - For other `format`s, any `type` can be used, typically for custom resources.
 *   - `name`: a scalar string specifying the file name as it was provided to **kana**.
 *
 *   If `embedded = true`, we additionally expect:
 *   - `offset`: a scalar integer specifying where the file starts as an offset from the start of the remaining bytes section.
 *     The offset for the first file should be zero, and entries in `files` should be ordered by increasing `offset`.
 *   - `size`: a non-negative scalar integer specifying the number of bytes in the file.
 *     The offset of each file should be equal to the sum of `size` and `offset` for the preceding file.
 *
 *   If `embedded = false`, we expect:
 *   - `id`: a scalar string containing some unique identifier for this file.
 *     The interpretation of `id` is application-specific but usually refers to some cache or database.
 * </details>
 * </DIV>
 *
 * <HR>
 * `results` should contain:
 * 
 * - `num_cells`: an integer scalar specifying the number of cells in the loaded dataset.
 * - `num_features`: a group containing integer scalar datasets, each named after a modality (typically `"RNA"` and/or `"ADT"`).
 *   Each dataset contains the number of features for its named modality.
 *   When dealing with multiple matrix inputs, the number of features is defined as the intersection across all matrices.
 * - `identities`: a group containin 1-dimensional integer datasets, each named after a modality in `num_features`.
 *   Each dataset is of length equal to the number of features listed in its corresponding `num_features` dataset.
 *   Each dataset contains the identities of the rows in the loaded dataset corresponding to its named modality.
 *   If a single input was provided, `identities` identifies each row in terms of its index in the "original" input matrix (i.e., if it were loaded without any modification).
 *   If multiple inputs were provided, `identities` contains the intersection of features across inputs, and each value refers to the row index in the original matrix of the _first_ input.
 *   Row identities are parallel to the per-feature results in subsequent analysis steps.
 *
 * <DIV style="color:blue">
 * <details>
 * <summary>For version 1.2</summary>
 * `results` should contain:
 * 
 * - `dimensions`: an integer dataset of length 2,
 *   containing the number of features and the number of cells in the loaded dataset.
 *   When dealing with multiple matrix inputs, the first entry is instead defined as the size of the intersection of features across all matrices.
 * - `identities`: a 1-dimensional integer dataset of length equal to the first entry of `dimensions`, containing the identity of each row in the loaded dataset.
 *   If a single input was provided, `identities` identifies each row in terms of its index in the "original" input matrix (i.e., if it were loaded without any modification).
 *   If multiple inputs were provided, `identities` contains the intersection of features across inputs, and each value refers to the row index in the original matrix of the _first_ input.
 *   Row identities are parallel to the per-gene results in subsequent analysis steps.
 *
 * `results` may also contain:
 *
 * - `num_samples`: an integer scalar dataset specifying the number of samples.
 *   If absent, this is assumed to be 1.
 *   For multiple matrices, the value listed here should be consistent with the number of samples specified in the paramaters.
 * </details>
 *
 * <details>
 * <summary>For version 1.1</summary>
 * `results` should contain:
 * 
 * - `dimensions`: an integer dataset of length 2,
 *   containing the number of features and the number of cells in the loaded dataset.
 *   When dealing with multiple matrix inputs, the first entry is instead defined as the size of the intersection of features across all matrices.
 *
 * `results` may also contain:
 *
 * - `num_samples`: an integer scalar dataset specifying the number of samples.
 *   If absent, this is assumed to be 1.
 *   For multiple matrices, the value listed here should be consistent with the number of samples specified in the paramaters.
 *
 * If there is only a single matrix, `results` should also contain:
 *
 * - `permutation`: an integer dataset of length equal to the number of cells,
 *   describing the permutation to be applied to the per-gene results to recover the original row order.
 *
 * If there are multiple matrices, `results` should instead contain:
 *
 * - `indices`: an integer dataset containing the row index of each feature in the intersection.
 *   For each entry, the gene is defined as the indexed row in the first sample _without permutation_.
 *   The `indices` are parallel to the per-gene results.
 * </details>
 *
 * <details>
 * <summary>For version 1.0</summary>
 * `results` should contain:
 * 
 * - `dimensions`: an integer dataset of length 2,
 *   containing the number of features and the number of cells in the loaded dataset.
 * - `permutation`: an integer dataset of length equal to the number of cells,
 *   describing the permutation to be applied to the per-gene results to recover the original row order.
 * </details>
 * </DIV>
 * 
 * All steps that generate per-gene results should use `identities` (or, for older formats, `permutation` or `indices`) to identify the genes corresponding to the statistics.
 * See the documentation for the functions listed below:
 *
 * - `feature_selection::validate()`
 * - `marker_detection::validate()`
 * - `custom_selections::validate()`
 * 
 * <HR>
 * @param handle An open HDF5 file handle.
 * @param embedded Whether the data files are embedded or linked.
 * @param version Version of the state file.
 *
 * @return Details about the dataset.
 * If the format is invalid, an error is raised instead.
 */
inline Details validate(const H5::Group& handle, bool embedded, int version) {
    auto ihandle = utils::check_and_open_group(handle, "inputs");

    ParamDump dump; 
    try {
        dump = validate_parameters(ihandle, embedded, version);
    } catch (std::exception& e) {
        throw utils::combine_errors(e, "failed to retrieve parameters from 'inputs'");
    }

    Details output;
    try {
        output = validate_results(ihandle, dump, version);
    } catch (std::exception& e) {
        throw utils::combine_errors(e, "failed to retrieve results from 'inputs'");
    }

    return output;
}

}

}

#endif
