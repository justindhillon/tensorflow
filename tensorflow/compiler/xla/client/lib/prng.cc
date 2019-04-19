/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/client/lib/prng.h"

#include <cmath>

#include "absl/base/casts.h"
#include "tensorflow/compiler/xla/client/lib/constants.h"
#include "tensorflow/compiler/xla/client/lib/math.h"
#include "tensorflow/compiler/xla/client/xla_builder.h"
#include "tensorflow/compiler/xla/util.h"

namespace xla {
namespace {

// Rotates a 32-bit integer 'v' left by 'distance' bits.
XlaOp RotateLeftU32(XlaOp v, int distance) {
  return (v << ConstantR0<uint32>(v.builder(), distance)) |
         ShiftRightLogical(v, ConstantR0<uint32>(v.builder(), 32 - distance));
}

// The internal state of the Three Fry implementation.
using ThreeFry2x32State = std::array<XlaOp, 2>;

// Implements the ThreeFry counter-based PRNG algorithm.
// Salmon et al. SC 2011. Parallel random numbers: as easy as 1, 2, 3.
// http://www.thesalmons.org/john/random123/papers/random123sc11.pdf
ThreeFry2x32State ThreeFry2x32(ThreeFry2x32State input, ThreeFry2x32State key) {
  XlaBuilder* builder = input[0].builder();
  key[0] = BitcastConvertType(key[0], U32);
  key[1] = BitcastConvertType(key[1], U32);

  // Rotation distances specified by the Threefry2x32 algorithm.
  constexpr std::array<int, 8> rotations = {13, 15, 26, 6, 17, 29, 16, 24};
  ThreeFry2x32State x;

  std::array<XlaOp, 3> ks;
  // 0x1BD11BDA is a parity constant specified by the ThreeFry2x32 algorithm.
  ks[2] = ConstantR0<uint32>(builder, 0x1BD11BDA);
  for (int i = 0; i < 2; ++i) {
    ks[i] = key[i];
    x[i] = input[i];
    ks[2] = ks[2] ^ key[i];
  }

  x[0] = x[0] + ks[0];
  x[1] = x[1] + ks[1];

  // Performs a single round of the Threefry2x32 algorithm, with a rotation
  // amount 'rotation'.
  auto round = [](ThreeFry2x32State v, int rotation) {
    v[0] = v[0] + v[1];
    v[1] = RotateLeftU32(v[1], rotation);
    v[1] = v[0] ^ v[1];
    return v;
  };

  // There are no known statistical flaws with 13 rounds of Threefry2x32.
  // We are conservative and use 20 rounds.
  x = round(x, rotations[0]);
  x = round(x, rotations[1]);
  x = round(x, rotations[2]);
  x = round(x, rotations[3]);
  x[0] = x[0] + ks[1];
  x[1] = x[1] + ks[2] + ConstantR0<uint32>(builder, 1);

  x = round(x, rotations[4]);
  x = round(x, rotations[5]);
  x = round(x, rotations[6]);
  x = round(x, rotations[7]);
  x[0] = x[0] + ks[2];
  x[1] = x[1] + ks[0] + ConstantR0<uint32>(builder, 2);

  x = round(x, rotations[0]);
  x = round(x, rotations[1]);
  x = round(x, rotations[2]);
  x = round(x, rotations[3]);
  x[0] = x[0] + ks[0];
  x[1] = x[1] + ks[1] + ConstantR0<uint32>(builder, 3);

  x = round(x, rotations[4]);
  x = round(x, rotations[5]);
  x = round(x, rotations[6]);
  x = round(x, rotations[7]);
  x[0] = x[0] + ks[1];
  x[1] = x[1] + ks[2] + ConstantR0<uint32>(builder, 4);

  x = round(x, rotations[0]);
  x = round(x, rotations[1]);
  x = round(x, rotations[2]);
  x = round(x, rotations[3]);
  x[0] = x[0] + ks[2];
  x[1] = x[1] + ks[0] + ConstantR0<uint32>(builder, 5);

  return x;
}

// Converts a uint64 to two uint32s.
std::array<XlaOp, 2> Uint64ToUint32s(XlaOp u64) {
  XlaBuilder* builder = u64.builder();
  XlaOp const32 = ConstantR0WithType(builder, U64, 32);
  XlaOp fst = ConvertElementType(u64, U32);
  XlaOp snd = ConvertElementType(ShiftRightLogical(u64, const32), U32);
  return {fst, snd};
}

// Converts two uint32s to a uint64.
XlaOp Uint32sToUint64(std::array<XlaOp, 2> u32s) {
  XlaBuilder* builder = u32s[0].builder();
  return ConvertElementType(u32s[0], U64) |
         ShiftLeft(ConvertElementType(u32s[1], U64),
                   ConstantR0WithType(builder, U64, 32));
}

// Given the initial state and the request number of random numbers to be
// generated, returns the input for the random number generator and a new state.
std::pair<ThreeFry2x32State, XlaOp> GetThreeFryInputsAndUpdatedState(
    XlaOp initial_state, const int64 size) {
  XlaBuilder* builder = initial_state.builder();
  XlaOp input_u64 = Iota(builder, U64, size);
  input_u64 = input_u64 + initial_state;
  XlaOp new_state = initial_state + ConstantR0<uint64>(builder, size);
  return std::make_pair(Uint64ToUint32s(input_u64), new_state);
}

// Generates random 32bits with the given shape using the Three Fry
// implementation. Returns the random bits and the new state.
RngOutput ThreeFryRngBit32(XlaOp key, XlaOp initial_state, const Shape& shape) {
  XlaBuilder* builder = key.builder();
  const int64 size = ShapeUtil::ElementsIn(shape);
  const int64 half_size = CeilOfRatio<int64>(size, 2);
  const bool size_is_odd = (half_size * 2 != size);
  std::pair<ThreeFry2x32State, XlaOp> inputs_state =
      GetThreeFryInputsAndUpdatedState(initial_state, half_size);
  ThreeFry2x32State inputs = inputs_state.first;
  ThreeFry2x32State outputs = ThreeFry2x32(inputs, Uint64ToUint32s(key));
  if (size_is_odd) {
    outputs[1] = Slice(outputs[1], {0}, {half_size - 1}, {1});
  }
  XlaOp result = ConcatInDim(builder, outputs, 0);
  return {Reshape(result, AsInt64Slice(shape.dimensions())),
          inputs_state.second};
}

// Generates random 64bits with the given shape using the Three Fry
// implementation. Returns the random bits and the new state.
RngOutput ThreeFryRngBit64(XlaOp key, XlaOp initial_state, const Shape& shape) {
  const int64 size = ShapeUtil::ElementsIn(shape);
  std::pair<ThreeFry2x32State, XlaOp> inputs_state =
      GetThreeFryInputsAndUpdatedState(initial_state, size);
  ThreeFry2x32State inputs = inputs_state.first;
  ThreeFry2x32State outputs = ThreeFry2x32(inputs, Uint64ToUint32s(key));
  XlaOp result = Uint32sToUint64(outputs);
  return {Reshape(result, AsInt64Slice(shape.dimensions())),
          inputs_state.second};
}

// The key of the Philox random number generator.
using Philox4x32Key = std::array<XlaOp, 2>;
// The internal state of the Philox random number generator.
using Philox4x32State = std::array<XlaOp, 4>;

// Computes the Philox4x32 algorithm using 10 rounds.
Philox4x32State Philox4x32(Philox4x32State state, Philox4x32Key key) {
  // Constants specified by the Philox algorithm.
  static const uint32 kPhiloxW32A = 0x9E3779B9;
  static const uint32 kPhiloxW32B = 0xBB67AE85;
  static const uint32 kPhiloxM4x32A = 0xD2511F53;
  static const uint32 kPhiloxM4x32B = 0xCD9E8D57;

  struct HighLowPair {
    XlaOp high;
    XlaOp low;
  };

  // Compute the high and low words from multiplying two 32-bit integers.
  auto mul_hi_low = [](XlaOp x, uint32 k) {
    auto product =
        ConvertElementType(x, U64) * ConstantR0<uint64>(x.builder(), k);
    auto low = ConvertElementType(product, U32);
    auto high =
        ConvertElementType(product >> ConstantR0<uint64>(x.builder(), 32), U32);
    return HighLowPair{high, low};
  };

  // Perform a single round of the Philox algorithm.
  auto philox_round = [&](Philox4x32State x, Philox4x32Key key) {
    auto product0 = mul_hi_low(x[0], kPhiloxM4x32A);
    auto product1 = mul_hi_low(x[2], kPhiloxM4x32B);
    return Philox4x32State{product1.high ^ x[1] ^ key[0], product1.low,
                           product0.high ^ x[3] ^ key[1], product0.low};
  };

  // Update the key after a round of Philox algorithm.
  auto raise_key = [](Philox4x32Key key) {
    XlaBuilder* builder = key[0].builder();
    return Philox4x32Key{key[0] + ConstantR0<uint32>(builder, kPhiloxW32A),
                         key[1] + ConstantR0<uint32>(builder, kPhiloxW32B)};
  };

  static const int kNumRounds = 10;
  for (int round = 0; round < kNumRounds; ++round, key = raise_key(key)) {
    state = philox_round(state, key);
  }
  return state;
}

// Scrambles the input key so that users don't need to worry about which part
// of the key needs to be strong.
std::pair<Philox4x32State, Philox4x32Key> GeneratePhiloxInternalStateAndKey(
    Philox4x32Key key) {
  XlaBuilder* builder = key[0].builder();
  XlaOp key0 = ConvertElementType(key[0], U64);
  XlaOp key1 = ConvertElementType(key[1], U64);

  Philox4x32State state = {
      ConvertElementType(key0, U32),
      ConvertElementType(key0 >> ScalarLike(key0, 32), U32),
      ConvertElementType(key1, U32),
      ConvertElementType(key1 >> ScalarLike(key1, 32), U32),
  };
  key = {ConstantR0<uint32>(builder, 0x3ec8f720),
         ConstantR0<uint32>(builder, 0x02461e29)};
  state = Philox4x32(state, key);
  XlaOp zero = ConstantR0<uint32>(builder, 0);
  return {Philox4x32State{zero, zero, state[2], state[3]},
          Philox4x32Key{state[0], state[1]}};
}

// Adds the integers [0, 1, ..., n) to 'state', treating 'state' as a 4 U32s, to
// compute n states for generating n random numbers.
Philox4x32State GetPhiloxGeneratorInputState(Philox4x32State state, int64 n) {
  XlaBuilder* builder = state[0].builder();
  XlaOp iota = Iota(builder, U64, n);
  XlaOp state_low = Uint32sToUint64({state[0], state[1]});
  XlaOp new_state_low = state_low + iota;
  std::array<XlaOp, 2> new_state_low_32s = Uint64ToUint32s(new_state_low);

  XlaOp one = ConstantR0<uint64>(builder, 1);
  XlaOp state_high = Uint32sToUint64({state[2], state[3]});
  XlaOp new_state_high =
      Select(Lt(new_state_low, state_low), Broadcast(state_high + one, {n}),
             Broadcast(state_high, {n}));
  std::array<XlaOp, 2> new_state_high_32s = Uint64ToUint32s(new_state_high);

  return {new_state_low_32s[0], new_state_low_32s[1], new_state_high_32s[0],
          new_state_high_32s[1]};
}

// Generates CeilOfRatio(num_elems, 4)*4 32bit Philox random numbers, as Philox
// numbers are generated in the unit of 128bits.
Philox4x32State GeneratePhiloxBits(int64 num_elems, Philox4x32Key key) {
  Philox4x32State state;
  std::tie(state, key) = GeneratePhiloxInternalStateAndKey(key);
  const int64 num_vector4 = CeilOfRatio<int64>(num_elems, 4);
  return Philox4x32(GetPhiloxGeneratorInputState(state, num_vector4), key);
}

// Generates an array of primitive type U32 with the given shape containing
// random bits generated by the Philox algorithm. Returns the array and the new
// state of the random number generator.
RngOutput PhiloxRngBit32(XlaOp op_key, XlaOp initial_state,
                         const Shape& shape) {
  XlaBuilder* builder = op_key.builder();
  const int64 num_elems = ShapeUtil::ElementsIn(shape);

  XlaOp new_state = initial_state + ConstantR0<uint64>(builder, num_elems);
  Philox4x32Key key = Uint64ToUint32s(op_key + initial_state);
  Philox4x32State state = GeneratePhiloxBits(num_elems, key);

  XlaOp numbers = ConcatInDim(builder, {state[0], state[1], state[2], state[3]},
                              /*dimension=*/0);
  numbers = Slice(numbers, /*start_indices=*/{0},
                  /*limit_indices=*/{num_elems},
                  /*strides=*/{1});
  return {Reshape(numbers, AsInt64Slice(shape.dimensions())), new_state};
}

// Generates an array of primitive type U64 with the given shape containing
// random bits generated by the Philox algorithm. Returns the array and the new
// state of the random number generator.
RngOutput PhiloxRngBit64(XlaOp op_key, XlaOp initial_state,
                         const Shape& shape) {
  XlaBuilder* builder = op_key.builder();
  const int64 num_elems = ShapeUtil::ElementsIn(shape);

  XlaOp new_state = initial_state + ConstantR0<uint64>(builder, num_elems);
  Philox4x32Key key = Uint64ToUint32s(op_key + initial_state);
  Philox4x32State state32 = GeneratePhiloxBits(num_elems * 2, key);

  auto convert_to_64 = [&](XlaOp v0, XlaOp v1) {
    return ConvertElementType(v0, U64) |
           ShiftLeft(ConvertElementType(v1, U64),
                     ConstantR0WithType(builder, U64, 32));
  };

  std::array<XlaOp, 2> state64;
  state64[0] = convert_to_64(state32[0], state32[1]);
  state64[1] = convert_to_64(state32[2], state32[3]);

  XlaOp numbers = ConcatInDim(builder, {state64[0], state64[1]},
                              /*dimension=*/0);
  numbers = Slice(numbers, /*start_indices=*/{0},
                  /*limit_indices=*/{num_elems},
                  /*strides=*/{1});
  return {Reshape(numbers, AsInt64Slice(shape.dimensions())), new_state};
}

XlaOp ConvertRandomBitsToUniformF32(XlaOp bits, XlaOp minval, XlaOp maxval) {
  XlaBuilder* builder = bits.builder();
  // Form 23 random mantissa bits, with a leading 1 bit. The leading 1 bit
  // forces the random bits into the mantissa.
  constexpr int kFloatBits = 32;
  constexpr int kMantissaBits = 23;
  bits = ShiftRightLogical(
             bits, ConstantR0<uint32>(builder, kFloatBits - kMantissaBits)) |
         ConstantR0<uint32>(builder, absl::bit_cast<uint32>(1.0f));
  XlaOp values = BitcastConvertType(bits, F32);

  // We have a floating point number in the range [1.0, 2.0).
  // Subtract 1.0f to shift to the range [0.0, 1.0)
  values = values - ConstantR0<float>(builder, 1.0f);
  // Multiply and add to shift to the range [minval, maxval).
  return values * (maxval - minval) + minval;
}

XlaOp ConvertRandomBitsToUniformInt(XlaOp bits, XlaOp minval, XlaOp maxval,
                                    PrimitiveType type,
                                    PrimitiveType unsigned_type) {
  XlaBuilder* builder = bits.builder();
  XlaOp range = BitcastConvertType(maxval, unsigned_type) -
                BitcastConvertType(minval, unsigned_type);
  XlaOp dist = Rem(bits, range);
  XlaOp dist_div_2 =
      ShiftRightLogical(dist, ConstantR0WithType(builder, unsigned_type, 1));

  return minval + BitcastConvertType(dist_div_2, type) +
         BitcastConvertType(dist - dist_div_2, type);
}

// Implements the Box-Muller transform, which converts random floats in the
// range of [0, 1] from uniform distribution to normal distribution with mean 0
// and variance 1. For more detail on the Box-Muller transform, see
// http://en.wikipedia.org/wiki/Box%E2%80%93Muller_transform#Basic_form
std::pair<XlaOp, XlaOp> BoxMullerTransform(XlaOp x0, XlaOp x1) {
  // Do not send a really small number to log().
  XlaOp u1 = Max(x0, ScalarLike(x0, 1.0e-7f));

  XlaOp v1 = ScalarLike(x1, 2.0f * M_PI) * x1;
  XlaOp u2 = Sqrt(ScalarLike(u1, -2.0f) * Log(u1));
  return {Sin(v1) * u2, Cos(v1) * u2};
}

}  // namespace

RngOutput ThreeFryBitGenerator(XlaOp key, XlaOp initial_state,
                               const Shape& shape) {
  PrimitiveType type = shape.element_type();
  switch (type) {
    case F32:
    case U32:
    case S32:
      return ThreeFryRngBit32(key, initial_state, shape);
    case U64:
    case S64:
      return ThreeFryRngBit64(key, initial_state, shape);
    default:
      return {key.builder()->ReportError(Unimplemented(
                  "Types other than F32, U32, S32, U64 and S64 "
                  "are not implemented by ThreeFryBitGenerator; got %s",
                  primitive_util::LowercasePrimitiveTypeName(type))),
              initial_state};
  }
}

RngOutput PhiloxBitGenerator(XlaOp key, XlaOp initial_state,
                             const Shape& shape) {
  PrimitiveType type = shape.element_type();
  switch (type) {
    case F32:
    case U32:
    case S32:
      return PhiloxRngBit32(key, initial_state, shape);
    case U64:
    case S64:
      return PhiloxRngBit64(key, initial_state, shape);
    default:
      return {key.builder()->ReportError(Unimplemented(
                  "Types other than F32, U32, S32, U64 and S64 "
                  "are not implemented by ThreeFryBitGenerator; got %s",
                  primitive_util::LowercasePrimitiveTypeName(type))),
              initial_state};
  }
}

RngOutput UniformF32Distribution(XlaOp key, XlaOp initial_state,
                                 BitGeneratorTy bit_generator, XlaOp minval,
                                 XlaOp maxval, const Shape& shape) {
  DCHECK_EQ(shape.element_type(), F32);
  RngOutput bits_state = bit_generator(key, initial_state, shape);
  XlaOp bits = bits_state.value;
  XlaOp new_state = bits_state.state;
  return {ConvertRandomBitsToUniformF32(bits, minval, maxval), new_state};
}

RngOutput UniformIntDistribution(XlaOp key, XlaOp initial_state,
                                 BitGeneratorTy bit_generator, XlaOp minval,
                                 XlaOp maxval, const Shape& shape) {
  RngOutput bits_state = bit_generator(key, initial_state, shape);
  XlaOp bits = bits_state.value;
  XlaOp new_state = bits_state.state;
  PrimitiveType type = shape.element_type();
  PrimitiveType unsigned_type;
  if (type == U32 || type == S32) {
    unsigned_type = U32;
  } else {
    DCHECK(type == U64 || type == S64);
    unsigned_type = U64;
  }
  return {
      ConvertRandomBitsToUniformInt(bits, minval, maxval, type, unsigned_type),
      new_state};
}

RngOutput NormalF32Distribution(XlaOp key, XlaOp initial_state,
                                BitGeneratorTy bit_generator,
                                const Shape& shape) {
  DCHECK_EQ(shape.element_type(), F32);
  XlaBuilder* builder = key.builder();
  const int64 num_elems = ShapeUtil::ElementsIn(shape);
  const int64 num_pairs = CeilOfRatio<int64>(num_elems, 2);
  RngOutput bits_state = UniformF32Distribution(
      key, initial_state, bit_generator, ConstantR0<float>(builder, 0.0),
      ConstantR0<float>(builder, 1.0),
      ShapeUtil::MakeShape(F32, {num_pairs * 2}));

  // Separate the bits into two groups to perform the Box-Muller transform.
  XlaOp bits_0 = Slice(bits_state.value, {0}, {num_pairs}, {1});
  XlaOp bits_1 = Slice(bits_state.value, {num_pairs}, {2 * num_pairs}, {1});
  std::tie(bits_0, bits_1) = BoxMullerTransform(bits_0, bits_1);

  // Put the numbers in the two groups back to form the requested shape.
  XlaOp normal = ConcatInDim(builder, {bits_0, bits_1}, /*dimension=*/0);
  if (num_elems != num_pairs * 2) {
    normal = Slice(normal, /*start_indices=*/{0}, /*limit_indices=*/{num_elems},
                   /*strides=*/{1});
  }
  normal = Reshape(normal, shape.dimensions());

  return {normal, bits_state.state};
}

}  // namespace xla
