#ifndef APACHE_BRIDGE_H
#define APACHE_BRIDGE_H

#include <arrow/api.h>
#include <avro/Compiler.hh>
#include <avro/DataFile.hh>
#include <avro/Decoder.hh>
#include <avro/Generic.hh>
#include <avro/ValidSchema.hh>
#include <avro/NodeImpl.hh>

#include <memory>
#include <string>
#include <vector>

namespace apache_bridge {

// Standalone schema conversion functions
arrow::Result<std::shared_ptr<arrow::Schema>> 
AvroSchemaToArrowSchema(const avro::ValidSchema& avro_schema);

avro::ValidSchema 
ArrowSchemaToAvroBatchSchema(const std::shared_ptr<arrow::Schema>& arrow_schema);

// Converter class for data conversion (requires memory pool)
class AvroToArrowConverter {
public:
    AvroToArrowConverter(avro::ValidSchema avro_batch_schema,
                         std::shared_ptr<arrow::Schema> arrow_schema)
    : pool_(arrow::default_memory_pool()),
      avro_batch_schema_(avro_batch_schema),
      arrow_schema_(arrow_schema) {

      }

    arrow::Result<std::shared_ptr<arrow::RecordBatch>>
    ConvertToRecordBatch(const std::vector<avro::GenericDatum>& avro_data);

private:
    arrow::Result<std::shared_ptr<arrow::DataType>>
    AvroTypeToArrowType(const avro::NodePtr& avro_type);

    arrow::Result<std::shared_ptr<arrow::Array>>
    BuildArrayFromAvroData(
        const avro::NodePtr& avro_type,
        const std::vector<avro::GenericDatum>& datums,
        size_t field_index,
        bool is_nullable);

    arrow::MemoryPool* pool_;
    avro::ValidSchema avro_batch_schema_;
    std::shared_ptr<arrow::Schema> arrow_schema_;
};

} // namespace apache_bridge

#endif // APACHE_BRIDGE_H