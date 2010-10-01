/* Copyright 2010 Jukka Jyl�nki

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */
#pragma once

/** @file BasicSerializedDataTypes.h
	@brief Describes the basic POD data types that are used by the DataSerializer and DataDeserializer objects. */

#include <list>
#include <string>

#include "Types.h"

namespace kNet
{

/// A single 0|1 value that is serialized as just one bit, *without padding*.
typedef bool bit;

/// Defines the basic data types that are the building blocks of every other serializable data type.
enum BasicSerializedDataType
{
	/// Denotes an invalid value of type BasicSerializedDataType.
	SerialInvalid = 0, 

	SerialBit, ///< Associated with the type 'bit'.
	SerialU8,  ///< Associated with the type 'u8'.
	SerialS8,  ///< Associated with the type 's8'.
	SerialU16, ///< Associated with the type 'u16'.
	SerialS16, ///< Associated with the type 's16'.
	SerialU32, ///< Associated with the type 'u32'.
	SerialS32, ///< Associated with the type 's32'.
	SerialU64, ///< Associated with the type 'u64'.
	SerialS64, ///< Associated with the type 's64'.
	SerialFloat,   ///< Associated with the type 'float'.
	SerialDouble,  ///< Associated with the type 'double'.
	SerialStruct,  ///< Not associated with a basic type, but defines that the given node contains an aggregate of multiple other types.
	SerialDynamicCount, ///< Not a data type, but an identifier for the SerializedMessageIterator to return when a dynamic count of a block is the next field to fill.

	/// The maximum number of different values for BasicSerializedDataType.
	NumSerialTypes
};

/// Converts a give BasicSerializedDataType to a readable string representation.
const char *SerialTypeToString(BasicSerializedDataType type);
/// Tries to parse a BasicSerializedDataType out of a string, or returns SerialInvalid if no match.
BasicSerializedDataType StringToSerialType(const char *type);
/// Returns the number of bytes the passed BasicSerializedDataType requires for serialization.
size_t SerialTypeSize(BasicSerializedDataType type);

/// A trait class used to match basic data types to their associated enum values.
template<typename T>
struct SerializedDataTypeTraits
{
	// The specializations each have to contain the following member: (The generic template version deliberately doesn't).
	// static const BasicSerializedDataType type = x;
};

template<> struct SerializedDataTypeTraits<bit> { static const BasicSerializedDataType type = SerialBit; };
template<> struct SerializedDataTypeTraits<u8>  { static const BasicSerializedDataType type = SerialU8; };
template<> struct SerializedDataTypeTraits<s8>  { static const BasicSerializedDataType type = SerialS8; };
template<> struct SerializedDataTypeTraits<u16> { static const BasicSerializedDataType type = SerialU16; };
template<> struct SerializedDataTypeTraits<s16> { static const BasicSerializedDataType type = SerialS16; };
template<> struct SerializedDataTypeTraits<u32> { static const BasicSerializedDataType type = SerialU32; };
template<> struct SerializedDataTypeTraits<s32> { static const BasicSerializedDataType type = SerialS32; };
template<> struct SerializedDataTypeTraits<u64> { static const BasicSerializedDataType type = SerialU64; };
template<> struct SerializedDataTypeTraits<s64> { static const BasicSerializedDataType type = SerialS64; };
template<> struct SerializedDataTypeTraits<float> { static const BasicSerializedDataType type = SerialFloat; };
template<> struct SerializedDataTypeTraits<double> { static const BasicSerializedDataType type = SerialDouble; };

} // ~kNet