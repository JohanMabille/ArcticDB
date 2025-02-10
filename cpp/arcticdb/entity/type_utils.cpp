/* Copyright 2024 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software
 * will be governed by the Apache License, version 2.0.
 */

#include <arcticdb/entity/type_utils.hpp>
#include <arcticdb/entity/types.hpp>

namespace arcticdb {
    bool trivially_compatible_types(const entity::TypeDescriptor& left, const entity::TypeDescriptor& right) {
        if (left == right)
            return true;

        // Multidimensional types are pointers
        if (left.dimension() >= entity::Dimension::Dim1 && right.dimension() >= entity::Dimension::Dim1)
            return true;

        // Multidimensional types are pointers the empty type is pointer as well
        if (left.dimension() >= entity::Dimension::Dim1 && is_empty_type(right.data_type()))
            return true;

        // Multidimensional types are pointers the empty type is pointer as well
        if (right.dimension() >= entity::Dimension::Dim1 && is_empty_type(left.data_type()))
            return true;

        if (is_sequence_type(left.data_type()) && is_sequence_type(right.data_type())) {
            // TODO coercion of utf strings is not always safe, should allow safe conversion and reinstate the
            // stronger requirement for trivial conversion below.
            //        if(!is_utf_type(slice_value_type(left.data_type)) &&
            //        !is_utf_type(slice_value_type(right.data_type)))
            //            return true;

            return is_utf_type(slice_value_type(left.data_type())) == is_utf_type(slice_value_type(right.data_type()));
        }

        return false;
    }

    constexpr bool is_mixed_float_and_integer(entity::DataType left, entity::DataType right) {
        return (is_integer_type(left) && is_floating_point_type(right)) || (is_floating_point_type(left) && is_integer_type(right));
    }

    std::optional<entity::TypeDescriptor> common_type_float_integer(const entity::TypeDescriptor& left, const entity::TypeDescriptor& right) {
        auto dimension = left.dimension();
        auto left_type = left.data_type();
        auto right_type = right.data_type();
        auto left_size = slice_bit_size(left_type);
        auto right_size = slice_bit_size(right_type);
        internal::check<ErrorCode::E_ASSERTION_FAILURE>(is_mixed_float_and_integer(left_type, right_type),
                                                        "Expected one int and one float in common_type_floats_integer");

        auto target_size = entity::SizeBits::UNKNOWN_SIZE_BITS;
        auto floating_size = is_floating_point_type(left_type) ? left_size : right_size;
        auto integral_size = is_floating_point_type(left_type) ? right_size : left_size;
        if (floating_size == entity::SizeBits::S64 || integral_size >= entity::SizeBits::S32) {
            // (u)int64 up to float64 will lose precision, we accept that
            target_size = entity::SizeBits::S64;
        } else {
            // (u)int(8/16) can fit in float32 since float32 has 24 precision bits
            target_size = entity::SizeBits::S32;
        }

        return std::make_optional<entity::TypeDescriptor>(combine_data_type(entity::ValueType::FLOAT, target_size), dimension);
    }

    std::optional<entity::TypeDescriptor> has_valid_type_promotion(
        const entity::TypeDescriptor& source,
        const entity::TypeDescriptor& target
    ) {

        if (source.dimension() != target.dimension()) {
            // Empty of dimension 0 means lack of any given type and can be promoted to anything (even if the dimensions
            // don't match), e.g. empty type can become int or array of ints. Empty type of higher dimension is used to
            // specify an empty array or an empty matrix, thus it cannot become any other type unless the dimensionality
            // matches
            if (is_empty_type(source.data_type()) && source.dimension() == entity::Dimension::Dim0)
                return target;
            return std::nullopt;
        }

        if (source == target)
            return target;

        // Empty type is coercible to any type
        if (is_empty_type(source.data_type())) {
            return target;
        }

        // Nothing is coercible to the empty type.
        if (is_empty_type(target.data_type())) {
            return std::nullopt;
        }

        auto source_type = source.data_type();
        auto target_type = target.data_type();
        auto source_size = slice_bit_size(source_type);
        auto target_size = slice_bit_size(target_type);

        if (is_time_type(source_type)) {
            if (!is_time_type(target_type))
                return std::nullopt;
        } else if (is_unsigned_type(source_type)) {
            if (is_unsigned_type(target_type)) {
                // UINT->UINT, target_size must be >= source_size
                if (source_size > target_size)
                    return std::nullopt;
            } else if (is_signed_type(target_type)) {
                // UINT->INT, target_size must be > source_size
                if (source_size >= target_size)
                    return std::nullopt;
            } else if (is_floating_point_type(target_type)) {
                // UINT->FLOAT
                // TODO aseaton is it OK to put this here? Where else is has_valid_type_promotion used? What will I break?
                // TODO put assertions in the places has_valid_type_promotion is used so I can see which code paths use it
                return common_type_float_integer(source, target);
            } else {
                // Non-numeric target type
                return std::nullopt;
            }
        } else if (is_signed_type(source_type)) {
            if (is_unsigned_type(target_type)) {
                // INT->UINT never promotable
                return std::nullopt;
            } else if (is_signed_type(target_type)) {
                // INT->INT, target_size must be >= source_size
                if (source_size > target_size)
                    return std::nullopt;
            } else if (is_floating_point_type(target_type)) {
                // INT->FLOAT
                // TODO aseaton is it OK to put this here? Where else is is_valid_type_promotion used? What will I break?
                return common_type_float_integer(source, target);
            } else {
                // Non-numeric target type
                return std::nullopt;
            }
        } else if (is_floating_point_type(source_type)) {
            if (is_unsigned_type(target_type) || is_signed_type(target_type)) {
                // FLOAT->U/INT never promotable
                return std::nullopt;
            } else if (is_floating_point_type(target_type)) {
                // FLOAT->FLOAT, target_size must be >= source_size
                if (source_size > target_size)
                    return std::nullopt;
            } else {
                // Non-numeric target type
                return std::nullopt;
            }
        } else if (is_sequence_type(source_type) && is_sequence_type(target_type)) {
            // Only allow promotion with UTF strings, and only to dynamic (never to fixed width)
            if (!is_utf_type(source_type) || !is_utf_type(target_type) || !is_dynamic_string_type(target_type))
                return std::nullopt;
        } else if (is_bool_object_type(source_type)) {
            return std::nullopt;
        } else {
            // Non-numeric source type
            return std::nullopt;
        }

        return target;
    }

    std::optional<entity::TypeDescriptor> common_type_mixed_sign_ints(const entity::TypeDescriptor& left, const entity::TypeDescriptor& right) {
        auto dimension = left.dimension();
        auto left_type = left.data_type();
        auto right_type = right.data_type();
        auto left_size = slice_bit_size(left_type);
        auto right_size = slice_bit_size(right_type);
        // To get here we must have one signed and one unsigned type, with the width of the signed type <= the width of the unsigned type
        internal::check<ErrorCode::E_ASSERTION_FAILURE>(is_signed_type(left_type) ^ is_signed_type(right_type),
                                                        "Expected one signed and one unsigned int in has_valid_common_type");
        if (is_signed_type(left_type)) {
            internal::check<ErrorCode::E_ASSERTION_FAILURE>(left_size <= right_size,
                                                            "Expected left_size <= right_size in has_valid_common_type");
        } else {
            // is_signed_type(right_type)
            internal::check<ErrorCode::E_ASSERTION_FAILURE>(right_size <= left_size,
                                                            "Expected right_size <= left_size in has_valid_common_type");
        }

        auto target_size = entity::SizeBits(uint8_t(std::max(left_size, right_size)) + 1);
        if (target_size < entity::SizeBits::COUNT) {
            return std::make_optional<entity::TypeDescriptor>(combine_data_type(entity::ValueType::INT, target_size), dimension);
        } else {
            return std::nullopt;
        }
    }

    std::optional<entity::TypeDescriptor> has_valid_common_type(const entity::TypeDescriptor& left, const entity::TypeDescriptor& right) {
            auto maybe_common_type = has_valid_type_promotion(left, right);
            if (!maybe_common_type) {
                maybe_common_type = has_valid_type_promotion(right, left);
            }
            // has_valid_type_promotion checks if the second type can represent all values of the first type
            // We also want to handle cases where there is a third type that can represent both
            if (!maybe_common_type && left.dimension() == right.dimension()) {
                if (is_integer_type(left.data_type()) && is_integer_type(right.data_type())) {
                    maybe_common_type = common_type_mixed_sign_ints(left, right);
                } else if (is_mixed_float_and_integer(left.data_type(), right.data_type())) {
                    maybe_common_type = common_type_float_integer(left, right);
                }
            }
            return maybe_common_type;
    }

}