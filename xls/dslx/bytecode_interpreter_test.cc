// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "xls/dslx/bytecode_interpreter.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/bytecode_emitter.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"

namespace xls::dslx {
namespace {

using status_testing::StatusIs;
using testing::HasSubstr;

// Interprets a nearly-minimal bytecode program; the same from
// BytecodeEmitterTest.SimpleTranslation.
TEST(BytecodeInterpreterTest, PositiveSmokeTest) {
  std::vector<Bytecode> bytecodes;

  Bytecode bc(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(1));
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kStore, 0);
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kLoad, 0);
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(2));
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kAdd);
  bytecodes.push_back(bc);

  std::vector<InterpValue> env;
  env.push_back(InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  EXPECT_EQ(value, InterpValue::MakeU32(3));
}

// Tests that a failing assert_eq is interpreted correctly. Again, a
// continuation of a test from BytecodeEmitterTest. Get used to it.
TEST(BytecodeInterpreterTest, AssertEqFail) {
  std::vector<Bytecode> bytecodes;
  Bytecode bc(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(3));
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kStore, 0);
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kLoad, 0);
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(2));
  bytecodes.push_back(bc);

  InterpValue fn_value(InterpValue::MakeFunction(Builtin::kAssertEq));
  bc = Bytecode(Span::Fake(), Bytecode::Op::kCall, fn_value);
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kStore, 1);
  bytecodes.push_back(bc);

  bc = Bytecode(Span::Fake(), Bytecode::Op::kLoad, 0);
  bytecodes.push_back(bc);

  std::vector<InterpValue> env;
  env.push_back(InterpValue::MakeUnit());
  env.push_back(InterpValue::MakeUnit());
  absl::StatusOr<InterpValue> value =
      BytecodeInterpreter::Interpret(bytecodes, &env);
  EXPECT_THAT(value.status(), StatusIs(absl::StatusCode::kInternal,
                                       HasSubstr("were not equal")));
}

// This test won't work unless BytecodeEmitterTest.DestructuringLet works!
TEST(BytecodeInterpreterTest, DestructuringLet) {
  constexpr absl::string_view kProgram = R"(#![test]
fn has_name_def_tree() -> (u32, u64, uN[128]) {
  let (a, b, (c, d)) = (u4:0, u8:1, (u16:2, (u32:3, u64:4, uN[128]:5)));
  let _ = assert_eq(a, u4:0);
  let _ = assert_eq(b, u8:1);
  let _ = assert_eq(c, u16:2);
  let _ = assert_eq(d, (u32:3, u64:4, uN[128]:5));
  d
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("has_name_def_tree"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(8, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  ASSERT_TRUE(value.IsTuple());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t num_elements, value.GetLength());
  ASSERT_EQ(num_elements, 3);
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue element,
                           value.Index(InterpValue::MakeU32(0)));
  XLS_ASSERT_OK_AND_ASSIGN(int64_t bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 3);

  XLS_ASSERT_OK_AND_ASSIGN(element, value.Index(InterpValue::MakeU32(1)));
  XLS_ASSERT_OK_AND_ASSIGN(bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 4);

  XLS_ASSERT_OK_AND_ASSIGN(element, value.Index(InterpValue::MakeU32(2)));
  XLS_ASSERT_OK_AND_ASSIGN(bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 5);
}

TEST(BytecodeInterpreterTest, RunTernaryConsequent) {
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           BytecodesFromString(
                               R"(000 literal u1:1
001 jump_rel_if +3
002 literal u32:64
003 jump_rel +3
004 jump_dest
005 literal u32:42
006 jump_dest)"));

  std::vector<InterpValue> env;
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  EXPECT_EQ(value, InterpValue::MakeU32(42)) << value.ToString();
}

TEST(BytecodeInterpreterTest, RunTernaryAlternate) {
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           BytecodesFromString(
                               R"(000 literal u1:0
001 jump_rel_if +3
002 literal u32:64
003 jump_rel +3
004 jump_dest
005 literal u32:42
006 jump_dest)"));

  std::vector<InterpValue> env;
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  EXPECT_EQ(value, InterpValue::MakeU32(64)) << value.ToString();
}

TEST(BytecodeInterpreterTest, BinopAnd) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_and() -> u32 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0xffffffff;
  a & b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_and"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xa5a5a5a5ll);
}

TEST(BytecodeInterpreterTest, BinopConcat) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_concat() -> u64 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0xffffffff;
  a ++ b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_concat"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xa5a5a5a5ffffffffll);
}

TEST(BytecodeInterpreterTest, BinopDiv) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_div() -> u32 {
  let a = u32:0x84208420;
  let b = u32:0x4;
  a / b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_div"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x21082108);
}

TEST(BytecodeInterpreterTest, BinopMul) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_mul() -> u32 {
  let a = u32:0x21082108;
  let b = u32:0x4;
  a * b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_mul"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x84208420);
}

TEST(BytecodeInterpreterTest, BinopOr) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_or() -> u32 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0x5a5a5a5a;
  a | b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_or"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xffffffff);
}

TEST(BytecodeInterpreterTest, BinopShll) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_shll() -> u32 {
  let a = u32:0x21082108;
  let b = u32:0x2;
  a << b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_shll"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x84208420);
}

TEST(BytecodeInterpreterTest, BinopShrl) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_shrl() -> u32 {
  let a = u32:0x84208420;
  let b = u32:0x2;
  a >> b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_shrl"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x21082108);
}

TEST(BytecodeInterpreterTest, BinopSub) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_sub() -> u32 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0x5a5a5a5a;
  a - b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_sub"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x4b4b4b4b);
}

TEST(BytecodeInterpreterTest, BinopXor) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_xor() -> u32 {
  let a = u32:0xa5a5ffff;
  let b = u32:0x5a5affff;
  a ^ b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_xor"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xffff0000);
}

TEST(BytecodeInterpreterTest, Unops) {
  constexpr absl::string_view kProgram = R"(#![test]
fn unops() -> s32 {
  let a = s32:1;
  let b = !a;
  -b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("unops"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x2);
}

}  // namespace
}  // namespace xls::dslx