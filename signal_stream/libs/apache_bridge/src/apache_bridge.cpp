#include "apache_bridge/apache_bridge.h"
#include <arrow/array/builder_primitive.h>

#include <boost/json.hpp>
#include <boost/json/src.hpp>

namespace apache_bridge {

// Helper function for type conversion (used by both standalone function and class)
static arrow::Result<std::shared_ptr<arrow::DataType>>
AvroTypeToArrowTypeImpl(const avro::NodePtr& avro_type) {
    switch (avro_type->type()) {
        case avro::AVRO_NULL:
            return arrow::null();
        case avro::AVRO_BOOL:
            return arrow::boolean();
        case avro::AVRO_INT:
            return arrow::int32();
        case avro::AVRO_LONG:
            return arrow::int64();
        case avro::AVRO_FLOAT:
            return arrow::float32();
        case avro::AVRO_DOUBLE:
            return arrow::float64();
        case avro::AVRO_STRING:
            return arrow::utf8();
        case avro::AVRO_BYTES:
            return arrow::binary();
        case avro::AVRO_FIXED:
            return arrow::fixed_size_binary(avro_type->fixedSize());
        case avro::AVRO_ENUM:
            return arrow::dictionary(arrow::int32(), arrow::utf8());
        case avro::AVRO_ARRAY: {
            ARROW_ASSIGN_OR_RAISE(auto value_type, AvroTypeToArrowTypeImpl(avro_type->leafAt(0)));
            return arrow::list(value_type);
        }
        case avro::AVRO_MAP: {
            ARROW_ASSIGN_OR_RAISE(auto value_type, AvroTypeToArrowTypeImpl(avro_type->leafAt(1)));
            return arrow::map(arrow::utf8(), value_type);
        }
        case avro::AVRO_UNION: {
            if (avro_type->leaves() == 2) {
                for (size_t i = 0; i < 2; ++i) {
                    if (avro_type->leafAt(i)->type() != avro::AVRO_NULL) {
                        return AvroTypeToArrowTypeImpl(avro_type->leafAt(i));
                    }
                }
            }
            std::vector<std::shared_ptr<arrow::Field>> union_fields;
            std::vector<int8_t> type_codes;
            for (size_t i = 0; i < avro_type->leaves(); ++i) {
                ARROW_ASSIGN_OR_RAISE(auto field_type, AvroTypeToArrowTypeImpl(avro_type->leafAt(i)));
                union_fields.push_back(arrow::field("union_" + std::to_string(i), field_type));
                type_codes.push_back(static_cast<int8_t>(i));
            }
            return arrow::sparse_union(union_fields, type_codes);
        }
        default:
            return arrow::Status::NotImplemented("Unsupported Avro type");
    }
}

arrow::Result<std::shared_ptr<arrow::DataType>>
AvroToArrowConverter::AvroTypeToArrowType(const avro::NodePtr& avro_type) {
    return AvroTypeToArrowTypeImpl(avro_type);
}

// Standalone function: Avro schema to Arrow schema
arrow::Result<std::shared_ptr<arrow::Schema>>
AvroSchemaToArrowSchema(const avro::ValidSchema& avro_schema) {
    const avro::NodePtr& root = avro_schema.root();
    
    if (root->type() != avro::AVRO_RECORD) {
        return arrow::Status::Invalid("Root schema must be a record");
    }
    
    std::vector<std::shared_ptr<arrow::Field>> arrow_fields;
    
    for (size_t i = 0; i < root->leaves(); ++i) {
        const avro::NodePtr& field_node = root->leafAt(i);
        ARROW_ASSIGN_OR_RAISE(auto arrow_type, AvroTypeToArrowTypeImpl(field_node));
        
        bool nullable = false;
        if (field_node->type() == avro::AVRO_UNION) {
            for (size_t j = 0; j < field_node->leaves(); ++j) {
                if (field_node->leafAt(j)->type() == avro::AVRO_NULL) {
                    nullable = true;
                    break;
                }
            }
        }
        
        arrow_fields.push_back(arrow::field(root->nameAt(i), arrow_type, nullable));
    }
    
    return arrow::schema(arrow_fields);
}

static const avro::GenericDatum& UnwrapNullable(const avro::GenericDatum& field, bool is_nullable, bool& is_null) {
    if (is_nullable && field.type() == avro::AVRO_UNION) {
        const avro::GenericDatum& inner = field.value<avro::GenericUnion>().datum();
        is_null = (inner.type() == avro::AVRO_NULL);
        return inner;
    }
    is_null = (field.type() == avro::AVRO_NULL);
    return field;
}

arrow::Result<std::shared_ptr<arrow::Array>>
AvroToArrowConverter::BuildArrayFromAvroData(
    const avro::NodePtr& avro_type,
    const std::vector<avro::GenericDatum>& datums,
    size_t field_index,
    bool is_nullable) {
    
    avro::NodePtr actual_type = avro_type;
    avro::LogicalType logical_type = actual_type->logicalType();

    if (is_nullable && avro_type->type() == avro::AVRO_UNION) {
        for (size_t i = 0; i < avro_type->leaves(); ++i) {
            if (avro_type->leafAt(i)->type() != avro::AVRO_NULL) {
                actual_type = avro_type->leafAt(i);
                break;
            }
        }
    }
    
    switch (actual_type->type()) {
        case avro::AVRO_BOOL: {
            arrow::BooleanBuilder builder(pool_);
            for (const auto& datum : datums) {
                const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                bool is_null;
                const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                if (is_null) {
                    ARROW_RETURN_NOT_OK(builder.AppendNull());
                } else {
                    ARROW_RETURN_NOT_OK(builder.Append(value.value<bool>()));
                }
            }
            return builder.Finish();
        }
        
        case avro::AVRO_INT: {
            arrow::Int32Builder builder(pool_);
            for (const auto& datum : datums) {
                const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                bool is_null;
                const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                if (is_null) {
                    ARROW_RETURN_NOT_OK(builder.AppendNull());
                } else {
                    ARROW_RETURN_NOT_OK(builder.Append(value.value<int32_t>()));
                }
            }
            return builder.Finish();
        }
        
        case avro::AVRO_LONG: {
            if (logical_type.type() == avro::LogicalType::TIMESTAMP_MILLIS || 
                logical_type.type() == avro::LogicalType::TIMESTAMP_MICROS ||
                logical_type.type() == avro::LogicalType::TIMESTAMP_NANOS) {
                
                arrow::TimeUnit::type unit;
                if (logical_type.type() == avro::LogicalType::TIMESTAMP_MILLIS) {
                    unit = arrow::TimeUnit::MILLI;
                }
                if (logical_type.type() == avro::LogicalType::TIMESTAMP_MICROS) {
                    unit = arrow::TimeUnit::MICRO;
                }
                if (logical_type.type() == avro::LogicalType::TIMESTAMP_NANOS) {
                    unit = arrow::TimeUnit::NANO;
                }

                auto timestamp_type = arrow::timestamp(unit);
                arrow::TimestampBuilder builder(timestamp_type, pool_);

                for (const auto& datum : datums) {
                    const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                    bool is_null;
                    const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                    
                    if (is_null) {
                        ARROW_RETURN_NOT_OK(builder.AppendNull());
                    } else {
                        // The underlying data in Avro is still an int64_t
                        ARROW_RETURN_NOT_OK(builder.Append(value.value<int64_t>()));
                    }
                }
                return builder.Finish();

            } else {
                arrow::Int64Builder builder(pool_);
                for (const auto& datum : datums) {
                    const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                    bool is_null;
                    const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                    
                    if (is_null) {
                        ARROW_RETURN_NOT_OK(builder.AppendNull());
                    } else {
                        ARROW_RETURN_NOT_OK(builder.Append(value.value<int64_t>()));
                    }
                }
                return builder.Finish();
            }
        }
        
        case avro::AVRO_FLOAT: {
            arrow::FloatBuilder builder(pool_);
            for (const auto& datum : datums) {
                const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                bool is_null;
                const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                if (is_null) {
                    ARROW_RETURN_NOT_OK(builder.AppendNull());
                } else {
                    ARROW_RETURN_NOT_OK(builder.Append(value.value<float>()));
                }
            }
            return builder.Finish();
        }
        
        case avro::AVRO_DOUBLE: {
            arrow::DoubleBuilder builder(pool_);
            for (const auto& datum : datums) {
                const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                bool is_null;
                const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                if (is_null) {
                    ARROW_RETURN_NOT_OK(builder.AppendNull());
                } else {
                    ARROW_RETURN_NOT_OK(builder.Append(value.value<double>()));
                }
            }
            return builder.Finish();
        }
        
        case avro::AVRO_STRING: {
            arrow::StringBuilder builder(pool_);
            for (const auto& datum : datums) {
                const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                bool is_null;
                const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                if (is_null) {
                    ARROW_RETURN_NOT_OK(builder.AppendNull());
                } else {
                    ARROW_RETURN_NOT_OK(builder.Append(value.value<std::string>()));
                }
            }
            return builder.Finish();
        }
        
        case avro::AVRO_BYTES: {
            arrow::BinaryBuilder builder(pool_);
            for (const auto& datum : datums) {
                const avro::GenericDatum& field = datum.value<avro::GenericRecord>().fieldAt(field_index);
                bool is_null;
                const avro::GenericDatum& value = UnwrapNullable(field, is_nullable, is_null);
                if (is_null) {
                    ARROW_RETURN_NOT_OK(builder.AppendNull());
                } else {
                    const auto& bytes = value.value<std::vector<uint8_t>>();
                    ARROW_RETURN_NOT_OK(builder.Append(bytes.data(), bytes.size()));
                }
            }
            return builder.Finish();
        }
        
        default:
            return arrow::Status::NotImplemented("Unsupported type for array building");
    }
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>>
AvroToArrowConverter::ConvertToRecordBatch(const std::vector<avro::GenericDatum>& avro_data) {

    if (avro_data.empty()) {
        return arrow::Status::Invalid("No data to convert");
    }

    const avro::NodePtr& root = avro_batch_schema_.root();

    // ---------------------------------------------------------------
    // Unwrap: if the schema root is AVRO_ARRAY, the record schema is
    // the array's single child (leafAt(0)).
    // ---------------------------------------------------------------
    avro::NodePtr record_node;
    if (root->type() == avro::AVRO_ARRAY) {
        record_node = root->leafAt(0);
        if (record_node->type() != avro::AVRO_RECORD) {
            return arrow::Status::Invalid(
                "Expected AVRO_RECORD as array item type, got type id: " +
                std::to_string(static_cast<int>(record_node->type())));
        }
    } else if (root->type() == avro::AVRO_RECORD) {
        record_node = root;
    } else {
        return arrow::Status::Invalid(
            "Unsupported root schema type id: " +
            std::to_string(static_cast<int>(root->type())));
    }

    // ---------------------------------------------------------------
    // Build Arrow arrays — one per field in the record
    // ---------------------------------------------------------------
    std::vector<std::shared_ptr<arrow::Array>> arrays;

    for (size_t i = 0; i < record_node->leaves(); ++i) {
        const avro::NodePtr& field_node = record_node->leafAt(i);

        bool nullable = false;
        if (field_node->type() == avro::AVRO_UNION) {
            for (size_t j = 0; j < field_node->leaves(); ++j) {
                if (field_node->leafAt(j)->type() == avro::AVRO_NULL) {
                    nullable = true;
                    break;
                }
            }
        }

        ARROW_ASSIGN_OR_RAISE(
            auto array,
            BuildArrayFromAvroData(field_node, avro_data, i, nullable));
        arrays.push_back(array);
    }

    return arrow::RecordBatch::Make(arrow_schema_,
                                     static_cast<int64_t>(avro_data.size()),
                                     arrays);
}

std::string EncodeToString(
        const std::shared_ptr<arrow::KeyValueMetadata>& meta) {
    if (!meta || meta->size() == 0) return "";
    boost::json::object obj;
    for (int64_t i = 0; i < meta->size(); ++i) {
        obj[meta->key(i)] = meta->value(i);
    }
    return boost::json::serialize(obj);
}

avro::NodePtr ArrowTypeToAvroNode(
        const std::shared_ptr<arrow::DataType>& arrow_type) {

    switch (arrow_type->id()) {
        case arrow::Type::BOOL:
            return avro::NodePtr(new avro::NodePrimitive(avro::AVRO_BOOL));

        case arrow::Type::INT8:
        case arrow::Type::UINT8:
        case arrow::Type::INT16:
        case arrow::Type::UINT16:
        case arrow::Type::INT32:
            return avro::NodePtr(new avro::NodePrimitive(avro::AVRO_INT));

        case arrow::Type::UINT32:
        case arrow::Type::INT64:
        case arrow::Type::UINT64:
            return avro::NodePtr(new avro::NodePrimitive(avro::AVRO_LONG));

        case arrow::Type::HALF_FLOAT:
        case arrow::Type::FLOAT:
            return avro::NodePtr(new avro::NodePrimitive(avro::AVRO_FLOAT));

        case arrow::Type::DOUBLE:
            return avro::NodePtr(new avro::NodePrimitive(avro::AVRO_DOUBLE));

        case arrow::Type::TIMESTAMP: {
            auto ts = std::static_pointer_cast<arrow::TimestampType>(arrow_type);
            auto node = avro::NodePtr(
                new avro::NodePrimitive(avro::AVRO_LONG));
            switch (ts->unit()) {
                case arrow::TimeUnit::SECOND:
                case arrow::TimeUnit::MILLI:
                    node->setLogicalType(
                        avro::LogicalType(avro::LogicalType::TIMESTAMP_MILLIS));
                    break;
                case arrow::TimeUnit::MICRO:
                    node->setLogicalType(
                        avro::LogicalType(avro::LogicalType::TIMESTAMP_MICROS));
                    break;
                case arrow::TimeUnit::NANO:
                    // No standard Avro logical type; raw long
                    break;
            }
            return node;
        }

        case arrow::Type::DATE32: {
            auto node = avro::NodePtr(
                new avro::NodePrimitive(avro::AVRO_INT));
            node->setLogicalType(
                avro::LogicalType(avro::LogicalType::DATE));
            return node;
        }

        case arrow::Type::DATE64: {
            auto node = avro::NodePtr(
                new avro::NodePrimitive(avro::AVRO_LONG));
            node->setLogicalType(
                avro::LogicalType(avro::LogicalType::TIMESTAMP_MILLIS));
            return node;
        }

        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING:
            return avro::NodePtr(
                new avro::NodePrimitive(avro::AVRO_STRING));

        case arrow::Type::BINARY:
        case arrow::Type::LARGE_BINARY:
        case arrow::Type::FIXED_SIZE_BINARY:
            return avro::NodePtr(
                new avro::NodePrimitive(avro::AVRO_BYTES));

        default:
            throw std::runtime_error(
                "Unsupported Arrow→Avro type id: " +
                std::to_string(static_cast<int>(arrow_type->id())));
    }
}

avro::ValidSchema ArrowSchemaToAvroBatchSchema(
        const std::shared_ptr<arrow::Schema>& arrow_schema) {

    // ── Build the record node ───────────────────────────────────────────
    auto record_node = avro::NodePtr(new avro::NodeRecord);

    // Set the record name FIRST, before adding any leaves/names.
    record_node->setName(avro::Name("ArrowRecord"));

    // ── Encode schema-level metadata + all arrow_type_ids into the
    //    record doc string as a single JSON object for retrieval. ────────
    boost::json::object doc_obj;

    // Schema-level metadata
    if (arrow_schema->metadata() && arrow_schema->metadata()->size() > 0) {
        boost::json::object schema_meta;
        const auto& meta = arrow_schema->metadata();
        for (int64_t m = 0; m < meta->size(); ++m) {
            schema_meta[meta->key(m)] = meta->value(m);
        }
        doc_obj["schema_metadata"] = std::move(schema_meta);
    }

    // Per-field auxiliary data (arrow_type_id + field metadata)
    boost::json::object fields_aux;

    for (int i = 0; i < arrow_schema->num_fields(); ++i) {
        const auto& field = arrow_schema->field(i);

        // Materialise the field name to guarantee its lifetime.
        std::string fname = field->name();

        // ── Build field type node ───────────────────────────────────────
        avro::NodePtr type_node = ArrowTypeToAvroNode(field->type());

        // ── Wrap in union if nullable ───────────────────────────────────
        if (field->nullable()) {
            auto union_node = avro::NodePtr(new avro::NodeUnion);
            union_node->addLeaf(
                avro::NodePtr(new avro::NodePrimitive(avro::AVRO_NULL)));
            union_node->addLeaf(type_node);
            record_node->addLeaf(union_node);
        } else {
            record_node->addLeaf(type_node);
        }

        record_node->addName(fname);

        // ── Collect auxiliary data for this field ───────────────────────
        boost::json::object field_aux;
        field_aux["arrow_type_id"] =
            static_cast<int>(field->type()->id());

        if (field->metadata() && field->metadata()->size() > 0) {
            boost::json::object fmeta;
            for (int64_t m = 0; m < field->metadata()->size(); ++m) {
                fmeta[field->metadata()->key(m)] =
                    field->metadata()->value(m);
            }
            field_aux["metadata"] = std::move(fmeta);
        }

        fields_aux[fname] = std::move(field_aux);
    }

    doc_obj["fields"] = std::move(fields_aux);

    // ── Set record doc ──────────────────────────────────────────────────
    record_node->setDoc(boost::json::serialize(doc_obj));

    // ── Build array node wrapping the record ────────────────────────────
    auto array_node = avro::NodePtr(new avro::NodeArray);
    array_node->addLeaf(record_node);

    return avro::ValidSchema(array_node);
}


} // namespace apache_bridge