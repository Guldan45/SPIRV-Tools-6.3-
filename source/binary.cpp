// Copyright (c) 2015 The Khronos Group Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and/or associated documentation files (the
// "Materials"), to deal in the Materials without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Materials, and to
// permit persons to whom the Materials are furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Materials.
//
// MODIFICATIONS TO THIS FILE MAY MEAN IT NO LONGER ACCURATELY REFLECTS
// KHRONOS STANDARDS. THE UNMODIFIED, NORMATIVE VERSIONS OF KHRONOS
// SPECIFICATIONS AND HEADER INFORMATION ARE LOCATED AT
//    https://www.khronos.org/registry/
//
// THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// MATERIALS OR THE USE OR OTHER DEALINGS IN THE MATERIALS.

#include <libspirv/libspirv.h>
#include "binary.h"
#include "diagnostic.h"
#include "ext_inst.h"
#include "opcode.h"
#include "operand.h"

#include <assert.h>
#include <string.h>

#include <sstream>

// Binary API

enum {
  I32_ENDIAN_LITTLE = 0x03020100ul,
  I32_ENDIAN_BIG = 0x00010203ul,
};

static const union {
  unsigned char bytes[4];
  uint32_t value;
} o32_host_order = {{0, 1, 2, 3}};

#define I32_ENDIAN_HOST (o32_host_order.value)

spv_result_t spvBinaryEndianness(const spv_binary binary,
                                 spv_endianness_t *pEndian) {
  spvCheck(!binary->code || !binary->wordCount,
           return SPV_ERROR_INVALID_BINARY);
  spvCheck(!pEndian, return SPV_ERROR_INVALID_POINTER);

  uint8_t bytes[4];
  memcpy(bytes, binary->code, sizeof(uint32_t));

  if (0x03 == bytes[0] && 0x02 == bytes[1] && 0x23 == bytes[2] &&
      0x07 == bytes[3]) {
    *pEndian = SPV_ENDIANNESS_LITTLE;
    return SPV_SUCCESS;
  }

  if (0x07 == bytes[0] && 0x23 == bytes[1] && 0x02 == bytes[2] &&
      0x03 == bytes[3]) {
    *pEndian = SPV_ENDIANNESS_BIG;
    return SPV_SUCCESS;
  }

  return SPV_ERROR_INVALID_BINARY;
}

uint32_t spvFixWord(const uint32_t word, const spv_endianness_t endian) {
  if ((SPV_ENDIANNESS_LITTLE == endian && I32_ENDIAN_HOST == I32_ENDIAN_BIG) ||
      (SPV_ENDIANNESS_BIG == endian && I32_ENDIAN_HOST == I32_ENDIAN_LITTLE)) {
    return (word & 0x000000ff) << 24 | (word & 0x0000ff00) << 8 |
           (word & 0x00ff0000) >> 8 | (word & 0xff000000) >> 24;
  }

  return word;
}

spv_result_t spvBinaryHeaderGet(const spv_binary binary,
                                const spv_endianness_t endian,
                                spv_header_t *pHeader) {
  spvCheck(!binary->code || !binary->wordCount,
           return SPV_ERROR_INVALID_BINARY);
  spvCheck(!pHeader, return SPV_ERROR_INVALID_POINTER);

  // TODO: Validation checking?
  pHeader->magic = spvFixWord(binary->code[SPV_INDEX_MAGIC_NUMBER], endian);
  pHeader->version = spvFixWord(binary->code[SPV_INDEX_VERSION_NUMBER], endian);
  pHeader->generator =
      spvFixWord(binary->code[SPV_INDEX_GENERATOR_NUMBER], endian);
  pHeader->bound = spvFixWord(binary->code[SPV_INDEX_BOUND], endian);
  pHeader->schema = spvFixWord(binary->code[SPV_INDEX_SCHEMA], endian);
  pHeader->instructions = &binary->code[SPV_INDEX_INSTRUCTION];

  return SPV_SUCCESS;
}

spv_result_t spvBinaryHeaderSet(spv_binary_t *binary, const uint32_t bound) {
  spvCheck(!binary, return SPV_ERROR_INVALID_BINARY);
  spvCheck(!binary->code || !binary->wordCount,
           return SPV_ERROR_INVALID_BINARY);

  binary->code[SPV_INDEX_MAGIC_NUMBER] = SPV_MAGIC_NUMBER;
  binary->code[SPV_INDEX_VERSION_NUMBER] = SPV_VERSION_NUMBER;
  binary->code[SPV_INDEX_GENERATOR_NUMBER] = SPV_GENERATOR_KHRONOS;
  binary->code[SPV_INDEX_BOUND] = bound;
  binary->code[SPV_INDEX_SCHEMA] = 0;  // NOTE: Reserved

  return SPV_SUCCESS;
}

spv_result_t spvBinaryEncodeU32(const uint32_t value, spv_instruction_t *pInst,
                                const spv_position position,
                                spv_diagnostic *pDiagnostic) {
  spvCheck(pInst->wordCount + 1 > SPV_LIMIT_INSTRUCTION_WORD_COUNT_MAX,
           DIAGNOSTIC << "Instruction word count '"
                      << SPV_LIMIT_INSTRUCTION_WORD_COUNT_MAX << "' exceeded.";
           return SPV_ERROR_INVALID_TEXT);

  pInst->words[pInst->wordCount++] = (uint32_t)value;
  return SPV_SUCCESS;
}

spv_result_t spvBinaryEncodeU64(const uint64_t value, spv_instruction_t *pInst,
                                const spv_position position,
                                spv_diagnostic *pDiagnostic) {
  spvCheck(pInst->wordCount + 2 > SPV_LIMIT_INSTRUCTION_WORD_COUNT_MAX,
           DIAGNOSTIC << "Instruction word count '"
                      << SPV_LIMIT_INSTRUCTION_WORD_COUNT_MAX << "' exceeded.";
           return SPV_ERROR_INVALID_TEXT);

  uint32_t low = (uint32_t)(0x00000000ffffffff & value);
  uint32_t high = (uint32_t)((0xffffffff00000000 & value) >> 32);
  pInst->words[pInst->wordCount++] = low;
  pInst->words[pInst->wordCount++] = high;
  return SPV_SUCCESS;
}

spv_result_t spvBinaryEncodeString(const char *str, spv_instruction_t *pInst,
                                   const spv_position position,
                                   spv_diagnostic *pDiagnostic) {
  size_t length = strlen(str);
  size_t wordCount = (length / 4) + 1;
  spvCheck((sizeof(uint32_t) * pInst->wordCount) + length >
               sizeof(uint32_t) * SPV_LIMIT_INSTRUCTION_WORD_COUNT_MAX,
           DIAGNOSTIC << "Instruction word count '"
                      << SPV_LIMIT_INSTRUCTION_WORD_COUNT_MAX << "'exceeded.";
           return SPV_ERROR_INVALID_TEXT);

  char *dest = (char *)&pInst->words[pInst->wordCount];
  strncpy(dest, str, length);
  pInst->wordCount += (uint16_t)wordCount;

  return SPV_SUCCESS;
}

spv_operand_type_t spvBinaryOperandInfo(const uint32_t word,
                                        const uint16_t operandIndex,
                                        const spv_opcode_desc opcodeEntry,
                                        const spv_operand_table operandTable,
                                        spv_operand_desc *pOperandEntry) {
  spv_operand_type_t type;
  if (operandIndex < opcodeEntry->wordCount) {
    // NOTE: Do operand table lookup to set operandEntry if successful
    uint16_t index = operandIndex - 1;
    type = opcodeEntry->operandTypes[index];
    spv_operand_desc entry = nullptr;
    if (!spvOperandTableValueLookup(operandTable, type, word, &entry)) {
      if (SPV_OPERAND_TYPE_NONE != entry->operandTypes[0]) {
        *pOperandEntry = entry;
      }
    }
  } else if (*pOperandEntry) {
    // NOTE: Use specified operand entry operand type for this word
    uint16_t index = operandIndex - opcodeEntry->wordCount;
    type = (*pOperandEntry)->operandTypes[index];
  } else if (OpSwitch == opcodeEntry->opcode) {
    // NOTE: OpSwitch is a special case which expects a list of paired extra
    // operands
    assert(0 &&
           "This case is previously untested, remove this assert and ensure it "
           "is behaving correctly!");
    uint16_t lastIndex = opcodeEntry->wordCount - 1;
    uint16_t index = lastIndex + ((operandIndex - lastIndex) % 2);
    type = opcodeEntry->operandTypes[index];
  } else {
    // NOTE: Default to last operand type in opcode entry
    uint16_t index = opcodeEntry->wordCount - 1;
    type = opcodeEntry->operandTypes[index];
  }
  return type;
}

spv_result_t spvBinaryDecodeOperand(
    const Op opcode, const spv_operand_type_t type, const uint32_t *words,
    const spv_endianness_t endian, const uint32_t options,
    const spv_operand_table operandTable, const spv_ext_inst_table extInstTable,
    spv_ext_inst_type_t *pExtInstType, out_stream &stream,
    spv_position position, spv_diagnostic *pDiagnostic) {
  spvCheck(!words || !position, return SPV_ERROR_INVALID_POINTER);
  spvCheck(!pDiagnostic, return SPV_ERROR_INVALID_DIAGNOSTIC);

  bool print = spvIsInBitfield(SPV_BINARY_TO_TEXT_OPTION_PRINT, options);
  bool color =
      print && spvIsInBitfield(SPV_BINARY_TO_TEXT_OPTION_COLOR, options);

  uint64_t index = 0;
  switch (type) {
    case SPV_OPERAND_TYPE_ID: {
      stream.get() << ((color) ? clr::yellow() : "");
      stream.get() << "%" << spvFixWord(words[index], endian);
      stream.get() << ((color) ? clr::reset() : "");
      index++;
      position->index++;
    } break;
    case SPV_OPERAND_TYPE_RESULT_ID: {
      stream.get() << (color ? clr::blue() : "");
      stream.get() << "%" << spvFixWord(words[index], endian);
      stream.get() << (color ? clr::reset() : "");
      index++;
      position->index++;
    } break;
    case SPV_OPERAND_TYPE_LITERAL: {
      // TODO: Need to support multiple word literals
      stream.get() << (color ? clr::red() : "");
      stream.get() << spvFixWord(words[index], endian);
      stream.get() << (color ? clr::reset() : "");
      index++;
      position->index++;
    } break;
    case SPV_OPERAND_TYPE_LITERAL_NUMBER: {
      // NOTE: Special case for extended instruction use
      if (OpExtInst == opcode) {
        spv_ext_inst_desc extInst;
        spvCheck(spvExtInstTableValueLookup(extInstTable, *pExtInstType,
                                            words[0], &extInst),
                 DIAGNOSTIC << "Invalid extended instruction '" << words[0]
                            << "'.";
                 return SPV_ERROR_INVALID_BINARY);
        stream.get() << (color ? clr::red() : "");
        stream.get() << extInst->name;
        stream.get() << (color ? clr::reset() : "");
      } else {
        stream.get() << (color ? clr::red() : "");
        stream.get() << spvFixWord(words[index], endian);
        stream.get() << (color ? clr::reset() : "");
      }
      index++;
      position->index++;
    } break;
    case SPV_OPERAND_TYPE_LITERAL_STRING: {
      const char *string = (const char *)&words[index];
      uint64_t stringOperandCount = (strlen(string) / 4) + 1;

      // NOTE: Special case for extended instruction import
      if (OpExtInstImport == opcode) {
        *pExtInstType = spvExtInstImportTypeGet(string);
        spvCheck(SPV_EXT_INST_TYPE_NONE == *pExtInstType,
                 DIAGNOSTIC << "Invalid extended instruction import'" << string
                            << "'.";
                 return SPV_ERROR_INVALID_BINARY);
      }

      stream.get() << "\"";
      stream.get() << (color ? clr::green() : "");
      stream.get() << string;
      stream.get() << (color ? clr::reset() : "");
      stream.get() << "\"";
      index += stringOperandCount;
      position->index += stringOperandCount;
    } break;
    case SPV_OPERAND_TYPE_CAPABILITY:
    case SPV_OPERAND_TYPE_SOURCE_LANGUAGE:
    case SPV_OPERAND_TYPE_EXECUTION_MODEL:
    case SPV_OPERAND_TYPE_ADDRESSING_MODEL:
    case SPV_OPERAND_TYPE_MEMORY_MODEL:
    case SPV_OPERAND_TYPE_EXECUTION_MODE:
    case SPV_OPERAND_TYPE_STORAGE_CLASS:
    case SPV_OPERAND_TYPE_DIMENSIONALITY:
    case SPV_OPERAND_TYPE_SAMPLER_ADDRESSING_MODE:
    case SPV_OPERAND_TYPE_SAMPLER_FILTER_MODE:
    case SPV_OPERAND_TYPE_FP_FAST_MATH_MODE:
    case SPV_OPERAND_TYPE_FP_ROUNDING_MODE:
    case SPV_OPERAND_TYPE_LINKAGE_TYPE:
    case SPV_OPERAND_TYPE_ACCESS_QUALIFIER:
    case SPV_OPERAND_TYPE_FUNCTION_PARAMETER_ATTRIBUTE:
    case SPV_OPERAND_TYPE_DECORATION:
    case SPV_OPERAND_TYPE_BUILT_IN:
    case SPV_OPERAND_TYPE_SELECTION_CONTROL:
    case SPV_OPERAND_TYPE_LOOP_CONTROL:
    case SPV_OPERAND_TYPE_FUNCTION_CONTROL:
    case SPV_OPERAND_TYPE_MEMORY_SEMANTICS:
    case SPV_OPERAND_TYPE_MEMORY_ACCESS:
    case SPV_OPERAND_TYPE_EXECUTION_SCOPE:
    case SPV_OPERAND_TYPE_GROUP_OPERATION:
    case SPV_OPERAND_TYPE_KERNEL_ENQ_FLAGS:
    case SPV_OPERAND_TYPE_KERNEL_PROFILING_INFO: {
      spv_operand_desc entry;
      spvCheck(
          spvOperandTableValueLookup(operandTable, type,
                                     spvFixWord(words[index], endian), &entry),
          DIAGNOSTIC << "Invalid " << spvOperandTypeStr(type) << " operand '"
                     << words[index] << "'.";
          return SPV_ERROR_INVALID_TEXT);
      stream.get() << entry->name;
      index++;
      position->index++;
    } break;
    default: {
      DIAGNOSTIC << "Invalid binary operand '" << type << "'";
      return SPV_ERROR_INVALID_BINARY;
    }
  }

  return SPV_SUCCESS;
}

spv_result_t spvBinaryDecodeOpcode(
    spv_instruction_t *pInst, const spv_endianness_t endian,
    const uint32_t options, const spv_opcode_table opcodeTable,
    const spv_operand_table operandTable, const spv_ext_inst_table extInstTable,
    out_stream &stream, spv_position position, spv_diagnostic *pDiagnostic) {
  spvCheck(!pInst || !position, return SPV_ERROR_INVALID_POINTER);
  spvCheck(!opcodeTable || !operandTable || !extInstTable,
           return SPV_ERROR_INVALID_TABLE);
  spvCheck(!pDiagnostic, return SPV_ERROR_INVALID_DIAGNOSTIC);

  uint16_t wordCount;
  Op opcode;
  spvOpcodeSplit(spvFixWord(pInst->words[0], endian), &wordCount, &opcode);

  spv_opcode_desc opcodeEntry;
  spvCheck(spvOpcodeTableValueLookup(opcodeTable, opcode, &opcodeEntry),
           DIAGNOSTIC << "Invalid Opcode '" << opcode << "'.";
           return SPV_ERROR_INVALID_BINARY);

  spvCheck(opcodeEntry->wordCount > wordCount,
           DIAGNOSTIC << "Invalid instruction word count '" << wordCount
                      << "', expected at least '" << opcodeEntry->wordCount
                      << "'.";
           return SPV_ERROR_INVALID_BINARY);

  std::stringstream no_result_id_strstream;
  out_stream no_result_id_stream(no_result_id_strstream);
  const int16_t result_id_index = spvOpcodeResultIdIndex(opcodeEntry);
  no_result_id_stream.get() << "Op" << opcodeEntry->name;

  position->index++;

  spv_operand_desc operandEntry = nullptr;
  for (uint16_t index = 1; index < wordCount; ++index) {
    const uint32_t word = spvFixWord(pInst->words[index], endian);
    const uint64_t currentPosIndex = position->index;

    if (result_id_index != index - 1) no_result_id_strstream << " ";
    spv_operand_type_t type = spvBinaryOperandInfo(word, index, opcodeEntry,
                                                   operandTable, &operandEntry);
    spvCheck(spvBinaryDecodeOperand(
                 opcodeEntry->opcode, type, pInst->words + index, endian,
                 options, operandTable, extInstTable, &pInst->extInstType,
                 (result_id_index == index - 1 ? stream : no_result_id_stream),
                 position, pDiagnostic),
             return SPV_ERROR_INVALID_BINARY);
    if (result_id_index == index - 1) stream.get() << " = ";
    index += (uint16_t)(position->index - currentPosIndex - 1);
  }

  stream.get() << no_result_id_strstream.str();

  return SPV_SUCCESS;
}

spv_result_t spvBinaryToText(const spv_binary binary, const uint32_t options,
                             const spv_opcode_table opcodeTable,
                             const spv_operand_table operandTable,
                             const spv_ext_inst_table extInstTable,
                             spv_text *pText, spv_diagnostic *pDiagnostic) {
  spvCheck(!binary->code || !binary->wordCount,
           return SPV_ERROR_INVALID_BINARY);
  spvCheck(!opcodeTable || !operandTable || !extInstTable,
           return SPV_ERROR_INVALID_TABLE);
  spvCheck(pText && spvIsInBitfield(SPV_BINARY_TO_TEXT_OPTION_PRINT, options),
           return SPV_ERROR_INVALID_POINTER);
  spvCheck(!pText && !spvIsInBitfield(SPV_BINARY_TO_TEXT_OPTION_PRINT, options),
           return SPV_ERROR_INVALID_POINTER);
  spvCheck(!pDiagnostic, return SPV_ERROR_INVALID_DIAGNOSTIC);

  spv_endianness_t endian;
  spv_position_t position = {};
  spvCheck(spvBinaryEndianness(binary, &endian),
           DIAGNOSTIC << "Invalid SPIR-V magic number '" << std::hex
                      << binary->code[0] << "'.";
           return SPV_ERROR_INVALID_BINARY);

  spv_header_t header;
  spvCheck(spvBinaryHeaderGet(binary, endian, &header),
           DIAGNOSTIC << "Invalid SPIR-V header.";
           return SPV_ERROR_INVALID_BINARY);

  bool print = spvIsInBitfield(SPV_BINARY_TO_TEXT_OPTION_PRINT, options);
  bool color =
      print && spvIsInBitfield(SPV_BINARY_TO_TEXT_OPTION_COLOR, options);

  std::stringstream sstream;
  out_stream stream(sstream);
  if (print) {
    stream = out_stream();
  }

  if (color) {
    stream.get() << clr::grey();
  }
  stream.get() << "; SPIR-V\n"
               << "; Version: " << header.version << "\n"
               << "; Generator: " << spvGeneratorStr(header.generator) << "\n"
               << "; Bound: " << header.bound << "\n"
               << "; Schema: " << header.schema << "\n";
  if (color) {
    stream.get() << clr::reset();
  }

  const uint32_t *words = binary->code;
  position.index = SPV_INDEX_INSTRUCTION;
  spv_ext_inst_type_t extInstType = SPV_EXT_INST_TYPE_NONE;
  while (position.index < binary->wordCount) {
    uint64_t index = position.index;
    uint16_t wordCount;
    Op opcode;
    spvOpcodeSplit(spvFixWord(words[position.index], endian), &wordCount,
                   &opcode);

    spv_instruction_t inst = {};
    inst.extInstType = extInstType;
    spvInstructionCopy(&words[position.index], opcode, wordCount, endian,
                       &inst);

    spvCheck(
        spvBinaryDecodeOpcode(&inst, endian, options, opcodeTable, operandTable,
                              extInstTable, stream, &position, pDiagnostic),
        return SPV_ERROR_INVALID_BINARY);
    extInstType = inst.extInstType;

    spvCheck((index + wordCount) != position.index,
             DIAGNOSTIC << "Invalid word count.";
             return SPV_ERROR_INVALID_BINARY);

    stream.get() << "\n";
  }

  if (!print) {
    size_t length = sstream.str().size();
    char *str = new char[length + 1];
    spvCheck(!str, return SPV_ERROR_OUT_OF_MEMORY);
    strncpy(str, sstream.str().c_str(), length + 1);
    spv_text text = new spv_text_t();
    spvCheck(!text, return SPV_ERROR_OUT_OF_MEMORY);
    text->str = str;
    text->length = length;
    *pText = text;
  }

  return SPV_SUCCESS;
}

void spvBinaryDestroy(spv_binary binary) {
  spvCheck(!binary, return );
  if (binary->code) {
    delete[] binary->code;
  }
  delete binary;
}
