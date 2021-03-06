#include <Storages/StorageFactory.h>
#include <Storages/StorageURL.h>

#include <Interpreters/Context.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Parsers/ASTLiteral.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadWriteBufferFromHTTP.h>
#include <IO/WriteBufferFromHTTP.h>
#include <IO/WriteHelpers.h>

#include <Formats/FormatFactory.h>

#include <DataStreams/IBlockOutputStream.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/AddingDefaultsBlockInputStream.h>

#include <Poco/Net/HTTPRequest.h>
#include <Processors/Sources/SourceWithProgress.h>
#include <Processors/Pipe.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

IStorageURLBase::IStorageURLBase(
    const Poco::URI & uri_,
    const Context & context_,
    const StorageID & table_id_,
    const String & format_name_,
    const ColumnsDescription & columns_,
    const ConstraintsDescription & constraints_,
    const String & compression_method_)
    : IStorage(table_id_)
    , uri(uri_)
    , context_global(context_)
    , compression_method(compression_method_)
    , format_name(format_name_)
{
    context_global.getRemoteHostFilter().checkURL(uri);
    setColumns(columns_);
    setConstraints(constraints_);
}

namespace
{
    class StorageURLSource : public SourceWithProgress
    {
    public:
        StorageURLSource(const Poco::URI & uri,
            const std::string & method,
            std::function<void(std::ostream &)> callback,
            const String & format,
            String name_,
            const Block & sample_block,
            const Context & context,
            const ColumnDefaults & column_defaults,
            UInt64 max_block_size,
            const ConnectionTimeouts & timeouts,
            const CompressionMethod compression_method)
            : SourceWithProgress(sample_block), name(std::move(name_))
        {
            read_buf = wrapReadBufferWithCompressionMethod(
                std::make_unique<ReadWriteBufferFromHTTP>(
                    uri,
                    method,
                    std::move(callback),
                    timeouts,
                    context.getSettingsRef().max_http_get_redirects,
                    Poco::Net::HTTPBasicCredentials{},
                    DBMS_DEFAULT_BUFFER_SIZE,
                    ReadWriteBufferFromHTTP::HTTPHeaderEntries{},
                    context.getRemoteHostFilter()),
                compression_method);

            reader = FormatFactory::instance().getInput(format, *read_buf, sample_block, context, max_block_size);
            reader = std::make_shared<AddingDefaultsBlockInputStream>(reader, column_defaults, context);
        }

        String getName() const override
        {
            return name;
        }

        Chunk generate() override
        {
            if (!reader)
                return {};

            if (!initialized)
                reader->readPrefix();

            initialized = true;

            if (auto block = reader->read())
                return Chunk(block.getColumns(), block.rows());

            reader->readSuffix();
            reader.reset();

            return {};
        }

    private:
        String name;
        std::unique_ptr<ReadBuffer> read_buf;
        BlockInputStreamPtr reader;
        bool initialized = false;
    };

    class StorageURLBlockOutputStream : public IBlockOutputStream
    {
    public:
        StorageURLBlockOutputStream(const Poco::URI & uri,
            const String & format,
            const Block & sample_block_,
            const Context & context,
            const ConnectionTimeouts & timeouts,
            const CompressionMethod compression_method)
            : sample_block(sample_block_)
        {
            write_buf = wrapWriteBufferWithCompressionMethod(
                std::make_unique<WriteBufferFromHTTP>(uri, Poco::Net::HTTPRequest::HTTP_POST, timeouts),
                compression_method, 3);
            writer = FormatFactory::instance().getOutput(format, *write_buf, sample_block, context);
        }

        Block getHeader() const override
        {
            return sample_block;
        }

        void write(const Block & block) override
        {
            writer->write(block);
        }

        void writePrefix() override
        {
            writer->writePrefix();
        }

        void writeSuffix() override
        {
            writer->writeSuffix();
            writer->flush();
            write_buf->finalize();
        }

    private:
        Block sample_block;
        std::unique_ptr<WriteBuffer> write_buf;
        BlockOutputStreamPtr writer;
    };
}


std::string IStorageURLBase::getReadMethod() const
{
    return Poco::Net::HTTPRequest::HTTP_GET;
}

std::vector<std::pair<std::string, std::string>> IStorageURLBase::getReadURIParams(const Names & /*column_names*/,
    const SelectQueryInfo & /*query_info*/,
    const Context & /*context*/,
    QueryProcessingStage::Enum & /*processed_stage*/,
    size_t /*max_block_size*/) const
{
    return {};
}

std::function<void(std::ostream &)> IStorageURLBase::getReadPOSTDataCallback(const Names & /*column_names*/,
    const SelectQueryInfo & /*query_info*/,
    const Context & /*context*/,
    QueryProcessingStage::Enum & /*processed_stage*/,
    size_t /*max_block_size*/) const
{
    return nullptr;
}


Pipes IStorageURLBase::read(const Names & column_names,
    const SelectQueryInfo & query_info,
    const Context & context,
    QueryProcessingStage::Enum processed_stage,
    size_t max_block_size,
    unsigned /*num_streams*/)
{
    auto request_uri = uri;
    auto params = getReadURIParams(column_names, query_info, context, processed_stage, max_block_size);
    for (const auto & [param, value] : params)
        request_uri.addQueryParameter(param, value);

    Pipes pipes;
    pipes.emplace_back(std::make_shared<StorageURLSource>(request_uri,
        getReadMethod(),
        getReadPOSTDataCallback(column_names, query_info, context, processed_stage, max_block_size),
        format_name,
        getName(),
        getHeaderBlock(column_names),
        context,
        getColumns().getDefaults(),
        max_block_size,
        ConnectionTimeouts::getHTTPTimeouts(context),
        chooseCompressionMethod(request_uri.getPath(), compression_method)));

    return pipes;
}

BlockOutputStreamPtr IStorageURLBase::write(const ASTPtr & /*query*/, const Context & /*context*/)
{
    return std::make_shared<StorageURLBlockOutputStream>(
        uri, format_name, getSampleBlock(), context_global,
        ConnectionTimeouts::getHTTPTimeouts(context_global),
        chooseCompressionMethod(uri.toString(), compression_method));
}

void registerStorageURL(StorageFactory & factory)
{
    factory.registerStorage("URL", [](const StorageFactory::Arguments & args)
    {
        ASTs & engine_args = args.engine_args;

        if (engine_args.size() != 2 && engine_args.size() != 3)
            throw Exception(
                "Storage URL requires 2 or 3 arguments: url, name of used format and optional compression method.", ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

        engine_args[0] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[0], args.local_context);

        String url = engine_args[0]->as<ASTLiteral &>().value.safeGet<String>();
        Poco::URI uri(url);

        engine_args[1] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[1], args.local_context);

        String format_name = engine_args[1]->as<ASTLiteral &>().value.safeGet<String>();

        String compression_method;
        if (engine_args.size() == 3)
        {
            engine_args[2] = evaluateConstantExpressionOrIdentifierAsLiteral(engine_args[2], args.local_context);
            compression_method = engine_args[2]->as<ASTLiteral &>().value.safeGet<String>();
        } else compression_method = "auto";

        return StorageURL::create(
            uri,
            args.table_id,
            format_name,
            args.columns, args.constraints, args.context,
            compression_method);
    });
}
}
