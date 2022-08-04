#include <Interpreters/DictionaryJoinAdapter.h>
#include <Interpreters/JoinUtils.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnConst.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionsExternalDictionaries.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

DictionaryJoinAdapter::DictionaryJoinAdapter(
    std::shared_ptr<const IDictionary> dictionary_, const Names & result_column_names)
    : IKeyValueEntity()
    , dictionary(dictionary_)
{
    if (!dictionary)
        throw Exception("Dictionary is not initialized", ErrorCodes::LOGICAL_ERROR);

    const auto & key_types = dictionary->getStructure().getKeyTypes();
    const auto & key_names = dictionary->getStructure().getKeysNames();
    if (key_types.size() != key_names.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Dictionary '{}' has invalid structure", dictionary->getFullName());

    for (size_t i = 0; i < key_types.size(); ++i)
    {
        sample_block.insert(ColumnWithTypeAndName(nullptr, key_types[i], key_names[i]));
    }

    for (const auto & attr_name : result_column_names)
    {
        const auto & attr = dictionary->getStructure().getAttribute(attr_name);

        sample_block.insert(ColumnWithTypeAndName(nullptr, attr.type, attr_name));

        attribute_names.emplace_back(attr_name);
        result_types.emplace_back(attr.type);
    }
}

Names DictionaryJoinAdapter::getPrimaryKey() const
{
    return dictionary->getStructure().getKeysNames();
}

Block DictionaryJoinAdapter::getSampleBlock() const
{
    return sample_block;
}

Chunk DictionaryJoinAdapter::getByKeys(const ColumnsWithTypeAndName & keys, PaddedPODArray<UInt8> & out_null_map) const
{
    if (keys.empty())
        return {};

    Columns key_columns;
    DataTypes key_types;
    for (const auto & key : keys)
    {
        key_columns.emplace_back(key.column);
        key_types.emplace_back(key.type);
    }

    {
        out_null_map.clear();

        auto mask = dictionary->hasKeys(key_columns, key_types);
        const auto & mask_data = mask->getData();

        out_null_map.resize(mask_data.size(), 0);
        std::copy(mask_data.begin(), mask_data.end(), out_null_map.begin());
    }

    Columns default_cols(result_types.size());
    for (size_t i = 0; i < result_types.size(); ++i)
        /// Dictinonary may have non-standart default values specified
        default_cols[i] = result_types[i]->createColumnConstWithDefaultValue(out_null_map.size());

    /// Result block consists of key columns and then attributes
    Columns result_columns = dictionary->getColumns(attribute_names, result_types, key_columns, key_types, default_cols);

    for (const auto & key_col : key_columns)
    {
        /// Insert default values for keys that were not found
        ColumnPtr filtered_key_col = JoinCommon::filterWithBlanks(key_col, out_null_map);
        result_columns.insert(result_columns.begin(), filtered_key_col);
    }

    size_t num_rows = result_columns[0]->size();
    return Chunk(std::move(result_columns), num_rows);
}

}
