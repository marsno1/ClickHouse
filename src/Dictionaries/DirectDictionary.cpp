#include "DirectDictionary.h"
#include <IO/WriteHelpers.h>
#include "DictionaryBlockInputStream.h"
#include "DictionaryFactory.h"
#include <Core/Defines.h>
#include <Functions/FunctionHelpers.h>
#include <Columns/ColumnNullable.h>
#include <DataTypes/DataTypesDecimal.h>
#include <Common/HashTable/HashSet.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int TYPE_MISMATCH;
    extern const int UNSUPPORTED_METHOD;
    extern const int BAD_ARGUMENTS;
}

namespace
{
    inline UInt64 getAt(const PaddedPODArray<UInt64> & arr, const size_t idx)
    {
        return arr[idx];
    }

    inline UInt64 getAt(const UInt64 & value, const size_t)
    {
        return value;
    }

    /// TODO: Use this class from DictionaryHelpers after cache dictionaries pull request will be merged
    template <DictionaryKeyType key_type>
    class DictionaryKeysExtractor
    {
    public:
        using KeyType = std::conditional_t<key_type == DictionaryKeyType::simple, UInt64, StringRef>;
        static_assert(key_type != DictionaryKeyType::range, "Range key type is not supported by DictionaryKeysExtractor");

        explicit DictionaryKeysExtractor(const Columns & key_columns, Arena & existing_arena)
        {
            assert(!key_columns.empty());

            if constexpr (key_type == DictionaryKeyType::simple)
                keys = getColumnVectorData(key_columns.front());
            else
                keys = deserializeKeyColumnsInArena(key_columns, existing_arena);
        }


        const PaddedPODArray<KeyType> & getKeys() const
        {
            return keys;
        }

    private:
        static PaddedPODArray<UInt64> getColumnVectorData(const ColumnPtr column)
        {
            PaddedPODArray<UInt64> result;

            auto full_column = column->convertToFullColumnIfConst();
            const auto *vector_col = checkAndGetColumn<ColumnVector<UInt64>>(full_column.get());

            if (!vector_col)
                throw Exception{ErrorCodes::TYPE_MISMATCH, "Column type mismatch for simple key expected UInt64"};

            result.assign(vector_col->getData());

            return result;
        }

        static PaddedPODArray<StringRef> deserializeKeyColumnsInArena(const Columns & key_columns, Arena & temporary_arena)
        {
            size_t keys_size = key_columns.front()->size();

            PaddedPODArray<StringRef> result;
            result.reserve(keys_size);

            PaddedPODArray<StringRef> temporary_column_data(key_columns.size());

            for (size_t key_index = 0; key_index < keys_size; ++key_index)
            {
                size_t allocated_size_for_columns = 0;
                const char * block_start = nullptr;

                for (size_t column_index = 0; column_index < key_columns.size(); ++column_index)
                {
                    const auto & column = key_columns[column_index];
                    temporary_column_data[column_index] = column->serializeValueIntoArena(key_index, temporary_arena, block_start);
                    allocated_size_for_columns += temporary_column_data[column_index].size;
                }

                result.push_back(StringRef{block_start, allocated_size_for_columns});
            }

            return result;
        }

        PaddedPODArray<KeyType> keys;
    };

    /// TODO: Use this class from DictionaryHelpers after cache dictionaries pull request will be merged
    class DefaultValueProvider final
    {
    public:
        explicit DefaultValueProvider(Field default_value_, ColumnPtr default_values_column_ = nullptr)
            : default_value(std::move(default_value_))
            , default_values_column(default_values_column_)
        {
        }


        Field getDefaultValue(size_t row) const
        {
            if (default_values_column)
                return (*default_values_column)[row];

            return default_value;
        }

    private:
        Field default_value;
        ColumnPtr default_values_column;
    };
}

template <DictionaryKeyType dictionary_key_type>
DirectDictionary<dictionary_key_type>::DirectDictionary(
    const StorageID & dict_id_,
    const DictionaryStructure & dict_struct_,
    DictionarySourcePtr source_ptr_,
    BlockPtr saved_block_)
    : IDictionary(dict_id_)
    , dict_struct(dict_struct_)
    , source_ptr{std::move(source_ptr_)}
    , saved_block{std::move(saved_block_)}
{
    if (!source_ptr->supportsSelectiveLoad())
        throw Exception{full_name + ": source cannot be used with DirectDictionary", ErrorCodes::UNSUPPORTED_METHOD};

    setup();
}

template <DictionaryKeyType dictionary_key_type>
void DirectDictionary<dictionary_key_type>::toParent(const PaddedPODArray<Key> & ids [[maybe_unused]], PaddedPODArray<Key> & out [[maybe_unused]]) const
{
    if constexpr (dictionary_key_type == DictionaryKeyType::simple)
    {
        const auto & attribute_name = hierarchical_attribute->name;

        auto result_type = std::make_shared<DataTypeUInt64>();
        auto input_column = result_type->createColumn();
        auto & input_column_typed = assert_cast<ColumnVector<UInt64> &>(*input_column);
        auto & data = input_column_typed.getData();
        data.insert(ids.begin(), ids.end());

        auto column = getColumn({attribute_name}, result_type, {std::move(input_column)}, {result_type}, {nullptr});
        const auto & result_column_typed = assert_cast<const ColumnVector<UInt64> &>(*column);
        const auto & result_data = result_column_typed.getData();

        out.assign(result_data);
    }
    else
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Hierarchy is not supported for complex key DirectDictionary");
}

template <DictionaryKeyType dictionary_key_type>
UInt64 DirectDictionary<dictionary_key_type>::getValueOrNullByKey(const Key & to_find) const
{
    std::vector<Key> required_key = {to_find};

    auto stream = source_ptr->loadIds(required_key);
    stream->readPrefix();

    bool is_found = false;
    UInt64 result = hierarchical_attribute->null_value.template get<UInt64>();

    while (const auto block = stream->read())
    {
        const IColumn & id_column = *block.safeGetByPosition(0).column;

        for (const size_t attribute_idx : ext::range(0, dict_struct.attributes.size()))
        {
            if (is_found)
                break;

            const IColumn & attribute_column = *block.safeGetByPosition(attribute_idx + 1).column;

            for (const auto row_idx : ext::range(0, id_column.size()))
            {
                const auto key = id_column[row_idx].get<UInt64>();

                if (key == to_find && hierarchical_attribute->name == attribute_name_by_index.at(attribute_idx))
                {
                    result = attribute_column[row_idx].get<Key>();
                    is_found = true;
                    break;
                }
            }
        }
    }

    stream->readSuffix();

    return result;
}

template <DictionaryKeyType dictionary_key_type>
template <typename ChildType, typename AncestorType>
void DirectDictionary<dictionary_key_type>::isInImpl(const ChildType & child_ids, const AncestorType & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    const auto null_value = hierarchical_attribute->null_value.template get<UInt64>();
    const auto rows = out.size();

    for (const auto row : ext::range(0, rows))
    {
        auto id = getAt(child_ids, row);
        const auto ancestor_id = getAt(ancestor_ids, row);

        for (size_t i = 0; id != null_value && id != ancestor_id && i < DBMS_HIERARCHICAL_DICTIONARY_MAX_DEPTH; ++i)
            id = getValueOrNullByKey(id);

        out[row] = id != null_value && id == ancestor_id;
    }

    query_count.fetch_add(rows, std::memory_order_relaxed);
}

template <DictionaryKeyType dictionary_key_type>
void DirectDictionary<dictionary_key_type>::isInVectorVector(
    const PaddedPODArray<UInt64> & child_ids, const PaddedPODArray<UInt64> & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    isInImpl(child_ids, ancestor_ids, out);
}

template <DictionaryKeyType dictionary_key_type>
void DirectDictionary<dictionary_key_type>::isInVectorConstant(const PaddedPODArray<UInt64> & child_ids, const UInt64 ancestor_id, PaddedPODArray<UInt8> & out) const
{
    isInImpl(child_ids, ancestor_id, out);
}

template <DictionaryKeyType dictionary_key_type>
void DirectDictionary<dictionary_key_type>::isInConstantVector(const UInt64 child_id, const PaddedPODArray<UInt64> & ancestor_ids, PaddedPODArray<UInt8> & out) const
{
    isInImpl(child_id, ancestor_ids, out);
}

template <DictionaryKeyType dictionary_key_type>
ColumnPtr DirectDictionary<dictionary_key_type>::getColumn(
        const std::string & attribute_name,
        const DataTypePtr & result_type,
        const Columns & key_columns,
        const DataTypes & key_types [[maybe_unused]],
        const ColumnPtr & default_values_column) const
{
    if constexpr (dictionary_key_type == DictionaryKeyType::complex)
        dict_struct.validateKeyTypes(key_types);

    Arena complex_key_arena;

    const DictionaryAttribute & attribute = dict_struct.getAttribute(attribute_name, result_type);
    auto result = attribute.type->createColumn();

    DefaultValueProvider default_value_provider(attribute.null_value, default_values_column);
    DictionaryKeysExtractor<dictionary_key_type> extractor(key_columns, complex_key_arena);
    const auto & requested_keys = extractor.getKeys();
    size_t requested_attribute_index = attribute_index_by_name.find(attribute_name)->second;

    size_t dictionary_keys_size = dict_struct.getKeysNames().size();
    size_t requested_key_index = 0;
    Field block_column_value;

    /** In result stream keys are returned in same order as they were requested.
      * For example if we request keys [1, 2, 3, 4] but source has only [2, 3] we need to return to client
      * [default_value, 2, 3, default_value].
      * For each key fetched from source current algorithm adds default values until
      * requested key with requested_key_index match key fetched from source.
      * At the end we also need to process tail.
      */

    BlockInputStreamPtr stream = getSourceBlockInputStream(key_columns, requested_keys);

    stream->readPrefix();

    while (const auto block = stream->read())
    {
        Columns block_key_columns;
        block_key_columns.reserve(dictionary_keys_size);

        auto block_columns = block.getColumns();

        /// Split into keys columns and attribute columns
        for (size_t i = 0; i < dictionary_keys_size; ++i)
        {
            block_key_columns.emplace_back(*block_columns.begin());
            block_columns.erase(block_columns.begin());
        }

        DictionaryKeysExtractor<dictionary_key_type> block_keys_extractor(block_key_columns, complex_key_arena);
        const auto & block_keys = block_keys_extractor.getKeys();
        size_t block_keys_size = block_keys.size();

        const auto & block_column = block.safeGetByPosition(dictionary_keys_size + requested_attribute_index).column;

        for (size_t block_key_index = 0; block_key_index < block_keys_size; ++block_key_index)
        {
            auto block_key = block_keys[block_key_index];

            while (requested_key_index < requested_keys.size() &&
                block_key != requested_keys[requested_key_index])
            {
                block_column_value = default_value_provider.getDefaultValue(requested_key_index);
                result->insert(block_column_value);
                ++requested_key_index;
            }

            block_column->get(block_key_index, block_column_value);
            result->insert(block_column_value);
            ++requested_key_index;
        }
    }

    stream->readSuffix();

    size_t requested_keys_size = requested_keys.size();

    Field default_value;
    /// Process tail, if source returned keys less keys sizes than we fetched insert default value for tail
    for (; requested_key_index < requested_keys_size; ++requested_key_index)
    {
        default_value = default_value_provider.getDefaultValue(requested_key_index);
        result->insert(default_value);
    }

    query_count.fetch_add(requested_keys_size, std::memory_order_relaxed);

    Field result_val;
    for (size_t i = 0; i < result->size(); ++i)
    {
        result->get(i, result_val);
        std::cerr << "I " << i << " dump " << result_val.dump() << std::endl;
    }

    return result;
}

template <DictionaryKeyType dictionary_key_type>
ColumnUInt8::Ptr DirectDictionary<dictionary_key_type>::hasKeys(const Columns & key_columns, const DataTypes & key_types [[maybe_unused]]) const
{
    if constexpr (dictionary_key_type == DictionaryKeyType::complex)
        dict_struct.validateKeyTypes(key_types);

    Arena complex_key_arena;

    DictionaryKeysExtractor<dictionary_key_type> requested_keys_extractor(key_columns, complex_key_arena);
    const auto & requested_keys = requested_keys_extractor.getKeys();
    size_t requested_keys_size = requested_keys.size();

    auto result = ColumnUInt8::create(requested_keys_size, false);
    auto & result_data = result->getData();

    size_t dictionary_keys_size = dict_struct.getKeysNames().size();
    size_t requested_key_index = 0;
    Field block_column_value;

    /** Algorithm is the same as in getColumn method. There are only 2 details
      * 1. We does not process tail because result column is created with false default value.
      * 2. If requested key does not match key from source we set false in requested_key_index.
      */

    BlockInputStreamPtr stream = getSourceBlockInputStream(key_columns, requested_keys);

    stream->readPrefix();

    while (const auto block = stream->read())
    {
        auto block_columns = block.getColumns();

        Columns block_key_columns;
        block_key_columns.reserve(dictionary_keys_size);

        /// Split into keys columns and attribute columns
        for (size_t i = 0; i < dictionary_keys_size; ++i)
        {
            block_key_columns.emplace_back(*block_columns.begin());
            block_columns.erase(block_columns.begin());
        }

        DictionaryKeysExtractor<dictionary_key_type> block_keys_extractor(block_key_columns, complex_key_arena);
        const auto & block_keys = block_keys_extractor.getKeys();
        size_t block_keys_size = block_keys.size();

        for (size_t block_key_index = 0; block_key_index < block_keys_size; ++block_key_index)
        {
            auto block_key = block_keys[block_key_index];

            while (requested_key_index < requested_keys.size() &&
                block_key != requested_keys[requested_key_index])
            {
                result_data[requested_key_index] = false;
                ++requested_key_index;
            }

            result_data[requested_key_index] = true;
            ++requested_key_index;
        }
    }

    stream->readSuffix();

    /// We does not add additional code for tail because result was initialized with false values

    query_count.fetch_add(requested_keys_size, std::memory_order_relaxed);

    return result;
}

template <DictionaryKeyType dictionary_key_type>
BlockInputStreamPtr DirectDictionary<dictionary_key_type>::getSourceBlockInputStream(
    const Columns & key_columns [[maybe_unused]],
    const PaddedPODArray<KeyType> & requested_keys [[maybe_unused]]) const
{
    size_t requested_keys_size = requested_keys.size();

    BlockInputStreamPtr stream;

    if constexpr (dictionary_key_type == DictionaryKeyType::simple)
    {
        std::vector<UInt64> ids;
        ids.reserve(requested_keys_size);

        for (auto key : requested_keys)
            ids.emplace_back(key);

        stream = source_ptr->loadIds(ids);
    }
    else
    {
        std::vector<size_t> requested_rows;
        requested_rows.reserve(requested_keys_size);
        for (size_t i = 0; i < requested_keys_size; ++i)
            requested_rows.emplace_back(i);

        stream = source_ptr->loadKeys(key_columns, requested_rows);
    }

    return stream;
}

template <DictionaryKeyType dictionary_key_type>
void DirectDictionary<dictionary_key_type>::setup()
{
    /// TODO: Move this to DictionaryStructure
    size_t dictionary_attributes_size = dict_struct.attributes.size();
    for (size_t i = 0; i < dictionary_attributes_size; ++i)
    {
        const auto & attribute = dict_struct.attributes[i];
        attribute_index_by_name[attribute.name] = i;
        attribute_name_by_index[i] = attribute.name;

        if (attribute.hierarchical)
        {
            if constexpr (dictionary_key_type == DictionaryKeyType::complex)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "({}): hierarchical attributes are not supported for complex key direct dictionary",
                    full_name);

            hierarchical_attribute = &attribute;

            if (attribute.underlying_type != AttributeUnderlyingType::utUInt64)
                throw Exception{full_name + ": hierarchical attribute must be UInt64.", ErrorCodes::TYPE_MISMATCH};
        }
    }
}

template <DictionaryKeyType dictionary_key_type>
BlockInputStreamPtr DirectDictionary<dictionary_key_type>::getBlockInputStream(const Names & /* column_names */, size_t /* max_block_size */) const
{
    return source_ptr->loadAll();
}

namespace
{
    template <DictionaryKeyType dictionary_key_type>
    DictionaryPtr createDirectDictionary(
        const std::string & full_name,
        const DictionaryStructure & dict_struct,
        const Poco::Util::AbstractConfiguration & config,
        const std::string & config_prefix,
        DictionarySourcePtr source_ptr)
    {
        const auto * layout_name = dictionary_key_type == DictionaryKeyType::simple ? "direct" : "complex_key_direct";

        if constexpr (dictionary_key_type == DictionaryKeyType::simple)
        {
            if (dict_struct.key)
                throw Exception(ErrorCodes::UNSUPPORTED_METHOD,
                    "'key' is not supported for dictionary of layout '({})'",
                    layout_name);
        }
        else
        {
            if (dict_struct.id)
                throw Exception(ErrorCodes::UNSUPPORTED_METHOD,
                    "'id' is not supported for dictionary of layout '({})'",
                    layout_name);
        }

        if (dict_struct.range_min || dict_struct.range_max)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "({}): elements .structure.range_min and .structure.range_max should be defined only " \
                "for a dictionary of layout 'range_hashed'",
                full_name);

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);

        if (config.has(config_prefix + ".lifetime.min") || config.has(config_prefix + ".lifetime.max"))
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "'lifetime' parameter is redundant for the dictionary' of layout '({})'",
                layout_name);

        return std::make_unique<DirectDictionary<dictionary_key_type>>(dict_id, dict_struct, std::move(source_ptr));
    }
}

template class DirectDictionary<DictionaryKeyType::simple>;
template class DirectDictionary<DictionaryKeyType::complex>;

void registerDictionaryDirect(DictionaryFactory & factory)
{
    factory.registerLayout("direct", createDirectDictionary<DictionaryKeyType::simple>, false);
    factory.registerLayout("complex_key_direct", createDirectDictionary<DictionaryKeyType::complex>, true);
}


}
