/* Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <arcticdb/entity/protobufs.hpp>
#include <arcticdb/entity/types.hpp>

#include <arcticdb/util/buffer.hpp>
#include <arcticdb/util/hash.hpp>
#include <arcticdb/log/log.hpp>

#include <type_traits>

namespace arcticdb::detail {
using namespace arcticdb::entity;

struct BlockProtobufHelper {
    std::size_t count_;
    std::size_t bytes_;

    void set_pb(arcticdb::proto::encoding::Block &block_pb, HashedValue h, std::size_t encoded_size) const {
        block_pb.set_in_bytes(static_cast<uint32_t>(bytes_));
        block_pb.set_out_bytes(static_cast<uint32_t>(encoded_size));
        block_pb.set_hash(h);
    }

    void set_version(arcticdb::proto::encoding::Block &block_pb, std::uint32_t version) const {
        block_pb.set_encoder_version(version);
    }
};

struct NdArrayBlock {
    std::size_t item_count_;
    BlockProtobufHelper shapes_;
    BlockProtobufHelper values_;

    void update_field_size(arcticdb::proto::encoding::NDArrayEncodedField &field) const {
        auto existing_items_count = field.items_count();
        field.set_items_count(existing_items_count + static_cast<std::uint32_t>(item_count_));
    }

    void set_pb(
        arcticdb::proto::encoding::Block* shapes_pb,
        arcticdb::proto::encoding::Block* values_pb,
        HashedValue shape_hash,
        std::size_t encoded_shape_bytes,
        HashedValue values_hash,
        std::size_t encoded_values_bytes
        ) {
        ARCTICDB_TRACE(log::codec(), "Setting encoded bytes: {}:{}", encoded_shape_bytes, encoded_values_bytes);
        shapes_.set_pb(*shapes_pb, shape_hash, encoded_shape_bytes);
        values_.set_pb(*values_pb, values_hash, encoded_values_bytes);
    }

    void set_version(
        arcticdb::proto::encoding::Block* shapes_pb,
        arcticdb::proto::encoding::Block* values_pb,
        std::uint32_t version,
        std::uint32_t shape_version) {
        shapes_.set_version(*shapes_pb, shape_version);
        values_.set_version(*values_pb, version);
    }
};

/**
 * Helper meant to mutualise boiler plate code that is going to exist regardless of the encoding
 * strategy
 * @tparam F field of dimension and type specified below
 * @tparam D Dimension tag class
 * @tparam DT DataType tag class
 */
template<class TD>
class CodecHelper {
  public:
    using DT = typename TD::DataTypeTag;
    using D = typename TD::DimensionTag;
    constexpr static HashedValue seed = 0x42;
    using T = typename DT::raw_type;
    constexpr static Dimension dim = D::value;

    CodecHelper() : hasher_(seed) {}

    HashAccum hasher_;

    void ensure_buffer(Buffer &out, std::ptrdiff_t pos, std::size_t bytes_count) {
        out.assert_size(pos + bytes_count);
    }

    HashedValue get_digest_and_reset() {
        HashedValue v = hasher_.digest();
        hasher_.reset(seed);
        return v;
    }

    static BlockProtobufHelper scalar_block(std::size_t elt) {
        return {elt, elt * sizeof(T)};
    }

    static NdArrayBlock nd_array_block(std::size_t elt, const shape_t *shape) {
        std::size_t shape_count = static_cast<std::size_t>(dim) * elt;
        std::size_t total_values_count = 0;
        if constexpr (dim == Dimension::Dim1) {
            for (std::size_t i = 0; i < shape_count; ++i, ++shape) {
                total_values_count += *shape;
            }
        } else if constexpr (dim == Dimension::Dim2) {
            for (std::size_t i = 0, end = shape_count / 2; i < end; ++i, shape += 2) {
                // this is specialized here for the Dim2 case
                total_values_count += *shape * *(shape + 1);
            }
        }
        std::size_t shape_bytes = shape_count * sizeof(shape_t);
        std::size_t data_bytes = total_values_count * sizeof(T);
        return NdArrayBlock{elt, {shape_count, shape_bytes}, {total_values_count, data_bytes}};
    }
};

/**
 * Turn a block encoding policy into a shape encoding one. This is essentially a convenience
 * type mapper. It is more optimal to implement a shape specific encoder to avoid default
 * option instantiation.
 * @tparam BE BlockEncoding see GenericBlockEncoder for more details
 * @tparam DefaultOpt {
 *  static void set_shape_default(Opts & opts);
 * }
 */
template<class BE, class DefaultOpt>
struct ShapeEncodingFromBlock {

    static constexpr decltype(BE::VERSION) VERSION = BE::VERSION;
    static decltype(BE::max_compressed_size(0)) max_compressed_size(std::size_t s) {
        return BE::max_compressed_size(s);
    }

    template<class T>
    static std::size_t encode_block(
        const T *in,
        BlockProtobufHelper &block_utils,
        HashAccum &hasher,
        T *out,
        std::size_t out_capacity,
        std::ptrdiff_t &pos,
        arcticdb::proto::encoding::VariantCodec &out_codec) {
        typename BE::Opts opts;
        DefaultOpt::set_shape_defaults(opts);
        return BE::encode_block(opts, in, block_utils, hasher, out, out_capacity, pos, out_codec);
    }
};

/**
 * This class is meant to simplify writing block based compression codecs by focusing on the block
 * encoding logic only. This might be suboptimal for codecs able to find patterns in dim > 0 but
 * removes a lot of boiler plate
 * @tparam F Field type (provider of actual data)
 * @tparam TD TypeDescriptorTag used to statically know the type and the dimension of the inputs
 * @tparam VE ValuesEncoding block implementation. Concept: {
 *  static std::size_t max_compressed_size(std::size_t);
 *  template<class T>
 *  static std::size_t encode_block(const Opts & opts, T * in, Block & block,
 *          HashAccum & hasher, T * out, std::size_t out_capacity, std::ptrdiff_t & pos,
 *          arcticdb::proto::encoding::VariantCodec & out_codec);
 *  static constexpr std::uint32_t VERSION;
 *  using Opts = VariantCodec::SomeCodecOptionType;
 * }
 * @tparam SE ShapeEncoding Same as VE but used for shape encoding. By default uses VE.
 */
template<class BlockType, class TD, class EncoderType, class SE = ShapeEncodingFromBlock<EncoderType, EncoderType>>
struct GenericBlockEncoder {
    using Helper = CodecHelper<TD>;
    using T = typename Helper::T;
    using ShapeEncoding = SE;

    GenericBlockEncoder() = delete;
    GenericBlockEncoder(GenericBlockEncoder &&enc) = delete;

    static size_t max_compressed_size(
        const BlockType &block
    ) {
        Helper helper;
        helper.hasher_.reset(helper.seed);
        std::size_t block_row_count = block.row_count();

        if constexpr (Helper::dim == Dimension::Dim0) {
            // Only store data, no shapes since dimension is 0
            auto helper_scalar_block = Helper::scalar_block(block_row_count);

            const auto uncompressed_size = helper_scalar_block.bytes_;
            auto compressed = EncoderType::max_compressed_size(uncompressed_size);
            ARCTICDB_TRACE(log::codec(), "Scalar block has {} bytes", compressed);
            return compressed;
        } else {
            auto helper_array_block = Helper::nd_array_block(block_row_count, block.shapes());

            std::size_t comp_data = EncoderType::max_compressed_size(helper_array_block.values_.bytes_);
            std::size_t comp_shapes = ShapeEncoding::max_compressed_size(helper_array_block.shapes_.bytes_);

            ARCTICDB_TRACE(log::codec(), "Array block has {} bytes ({} + {})", comp_data + comp_shapes, comp_shapes, comp_data);
            return comp_data + comp_shapes;
        }
    }

    static void encode(
        const typename EncoderType::Opts &opts,
        BlockType &block,
        arcticdb::proto::encoding::EncodedField &field,
        Buffer &out,
        std::ptrdiff_t &pos
        ) {
        Helper helper;
        helper.hasher_.reset(helper.seed);
        std::size_t block_row_count = block.row_count();

        if constexpr (Helper::dim == Dimension::Dim0) {
            // Only store data, no shapes since dimension is 0
            auto helper_scalar_block = Helper::scalar_block(block_row_count);
            ARCTICDB_TRACE(log::codec(), "Generic block encode writing scalar of {} elements", block_row_count);

            const auto uncompressed_size = helper_scalar_block.bytes_;
            std::size_t max_compressed_size = EncoderType::max_compressed_size(uncompressed_size);
            helper.ensure_buffer(out, pos, max_compressed_size);

            // doing copy + hash in one pass, this might have a negative effect on perf
            // since the hashing is path dependent. This is a toy example though so not critical
            auto t_out = reinterpret_cast<T *>(out.data() + pos);
            auto *field_nd_array = field.mutable_ndarray();
            const auto total_items_count = field_nd_array->items_count() + block_row_count;
            field_nd_array->set_items_count(total_items_count);
            auto value_pb = field_nd_array->add_values();

           const auto compressed_size = EncoderType::encode_block(opts, block.data(), helper_scalar_block, helper.hasher_, t_out,
                                                           max_compressed_size, pos, *value_pb->mutable_codec());

            helper_scalar_block.set_pb(*value_pb, helper.hasher_.digest(), compressed_size);
            helper_scalar_block.set_version(*value_pb, EncoderType::VERSION);
        } else {
            auto helper_array_block = Helper::nd_array_block(block_row_count, block.shapes());

            ARCTICDB_TRACE(log::codec(), "Generic block encoder writing ndarray field of {} items", helper_array_block.item_count_);
            std::size_t comp_data = EncoderType::max_compressed_size(helper_array_block.values_.bytes_);
            std::size_t comp_shapes = ShapeEncoding::max_compressed_size(helper_array_block.shapes_.bytes_);

            helper.ensure_buffer(out, pos, comp_data + comp_shapes);
            auto field_nd_array = field.mutable_ndarray();
            auto shape_pb = field_nd_array->add_shapes();

            // write shapes
            auto s_out = reinterpret_cast<shape_t *>(out.data() + pos);
            const auto shape_comp_size = ShapeEncoding::encode_block(block.shapes(),
                                                                      helper_array_block.shapes_,
                                                                      helper.hasher_,
                                                                      s_out,
                                                                      comp_shapes,
                                                                      pos,
                                                                      *shape_pb->mutable_codec());
            HashedValue shape_hash = helper.get_digest_and_reset();

            // write values
            auto value_pb = field_nd_array->add_values();
            auto t_out = reinterpret_cast<T *>(out.data() + pos);
            const auto values_comp_size = EncoderType::encode_block(opts,
                                                                        block.data(),
                                                                        helper_array_block.values_,
                                                                        helper.hasher_,
                                                                        t_out,
                                                                        comp_data,
                                                                        pos,
                                                                        *value_pb->mutable_codec());

            auto digest = helper.hasher_.digest();
            helper_array_block.update_field_size(*field_nd_array);
            helper_array_block.set_pb(shape_pb, value_pb, shape_hash, shape_comp_size, digest, values_comp_size);
            helper_array_block.set_version(shape_pb, value_pb, EncoderType::VERSION, ShapeEncoding::VERSION);
        }
    }
};

} // namespace arcticdb::detail
