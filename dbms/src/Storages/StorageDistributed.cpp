#include <DB/DataStreams/RemoteBlockInputStream.h>
#include <DB/DataStreams/BlockExtraInfoInputStream.h>
#include <DB/DataStreams/UnionBlockInputStream.h>

#include <DB/Databases/IDatabase.h>

#include <DB/Storages/StorageDistributed.h>
#include <DB/Storages/VirtualColumnFactory.h>
#include <DB/Storages/Distributed/DistributedBlockOutputStream.h>
#include <DB/Storages/Distributed/DirectoryMonitor.h>
#include <DB/Storages/MergeTree/ReshardingWorker.h>

#include <DB/Common/escapeForFileName.h>

#include <DB/Parsers/ASTInsertQuery.h>
#include <DB/Parsers/ASTSelectQuery.h>
#include <DB/Parsers/ASTIdentifier.h>
#include <DB/Parsers/TablePropertiesQueriesASTs.h>
#include <DB/Parsers/ParserAlterQuery.h>
#include <DB/Parsers/parseQuery.h>
#include <DB/Parsers/ASTWeightedZooKeeperPath.h>
#include <DB/Parsers/ASTLiteral.h>

#include <DB/Interpreters/InterpreterSelectQuery.h>
#include <DB/Interpreters/InterpreterAlterQuery.h>
#include <DB/Interpreters/InterpreterDescribeQuery.h>
#include <DB/Interpreters/ExpressionAnalyzer.h>
#include <DB/Interpreters/ClusterProxy/Query.h>
#include <DB/Interpreters/ClusterProxy/SelectQueryConstructor.h>
#include <DB/Interpreters/ClusterProxy/DescribeQueryConstructor.h>
#include <DB/Interpreters/ClusterProxy/AlterQueryConstructor.h>

#include <DB/Core/Field.h>

#include <memory>

namespace DB
{

namespace ErrorCodes
{
	extern const int STORAGE_REQUIRES_PARAMETER;
	extern const int RESHARDING_NO_WORKER;
	extern const int RESHARDING_INVALID_PARAMETERS;
	extern const int RESHARDING_INITIATOR_CHECK_FAILED;
}


namespace
{
	/// select query has database and table names as AST pointers
	/// Создает копию запроса, меняет имена базы данных и таблицы.
	inline ASTPtr rewriteSelectQuery(const ASTPtr & query, const std::string & database, const std::string & table)
	{
		auto modified_query_ast = query->clone();

		auto & actual_query = typeid_cast<ASTSelectQuery &>(*modified_query_ast);
		actual_query.database = std::make_shared<ASTIdentifier>({}, database, ASTIdentifier::Database);
		actual_query.table = std::make_shared<ASTIdentifier>({}, table, ASTIdentifier::Table);

		return modified_query_ast;
	}

	/// insert query has database and table names as bare strings
	/// Создает копию запроса, меняет имена базы данных и таблицы.
	inline ASTPtr rewriteInsertQuery(const ASTPtr & query, const std::string & database, const std::string & table)
	{
		auto modified_query_ast = query->clone();

		auto & actual_query = typeid_cast<ASTInsertQuery &>(*modified_query_ast);
		actual_query.database = database;
		actual_query.table = table;
		/// make sure query is not INSERT SELECT
		actual_query.select = nullptr;

		return modified_query_ast;
	}
}


StorageDistributed::StorageDistributed(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	const Cluster & cluster_,
	Context & context_,
	const ASTPtr & sharding_key_,
	const String & data_path_)
	: name(name_), columns(columns_),
	remote_database(remote_database_), remote_table(remote_table_),
	context(context_), cluster(cluster_),
	sharding_key_expr(sharding_key_ ? ExpressionAnalyzer(sharding_key_, context, nullptr, *columns).getActions(false) : nullptr),
	sharding_key_column_name(sharding_key_ ? sharding_key_->getColumnName() : String{}),
	write_enabled(!data_path_.empty() && (((cluster.getLocalShardCount() + cluster.getRemoteShardCount()) < 2) || sharding_key_)),
	path(data_path_.empty() ? "" : (data_path_ + escapeForFileName(name) + '/'))
{
	createDirectoryMonitors();
}

StorageDistributed::StorageDistributed(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const NamesAndTypesList & materialized_columns_,
	const NamesAndTypesList & alias_columns_,
	const ColumnDefaults & column_defaults_,
	const String & remote_database_,
	const String & remote_table_,
	const Cluster & cluster_,
	Context & context_,
	const ASTPtr & sharding_key_,
	const String & data_path_)
	: IStorage{materialized_columns_, alias_columns_, column_defaults_},
	name(name_), columns(columns_),
	remote_database(remote_database_), remote_table(remote_table_),
	context(context_), cluster(cluster_),
	sharding_key_expr(sharding_key_ ? ExpressionAnalyzer(sharding_key_, context, nullptr, *columns).getActions(false) : nullptr),
	sharding_key_column_name(sharding_key_ ? sharding_key_->getColumnName() : String{}),
	write_enabled(!data_path_.empty() && (((cluster.getLocalShardCount() + cluster.getRemoteShardCount()) < 2) || sharding_key_)),
	path(data_path_.empty() ? "" : (data_path_ + escapeForFileName(name) + '/'))
{
	createDirectoryMonitors();
}

StoragePtr StorageDistributed::create(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const NamesAndTypesList & materialized_columns_,
	const NamesAndTypesList & alias_columns_,
	const ColumnDefaults & column_defaults_,
	const String & remote_database_,
	const String & remote_table_,
	const String & cluster_name,
	Context & context_,
	const ASTPtr & sharding_key_,
	const String & data_path_)
{
	return (new StorageDistributed{
		name_, columns_,
		materialized_columns_, alias_columns_, column_defaults_,
		remote_database_, remote_table_,
		context_.getCluster(cluster_name), context_,
		sharding_key_, data_path_
	})->thisPtr();
}


StoragePtr StorageDistributed::create(
	const std::string & name_,
	NamesAndTypesListPtr columns_,
	const String & remote_database_,
	const String & remote_table_,
	std::shared_ptr<Cluster> & owned_cluster_,
	Context & context_)
{
	auto res = new StorageDistributed{
		name_, columns_, remote_database_,
		remote_table_, *owned_cluster_, context_
	};

	/// Захватываем владение объектом-кластером.
	res->owned_cluster = owned_cluster_;

	return res->thisPtr();
}

BlockInputStreams StorageDistributed::read(
	const Names & column_names,
	ASTPtr query,
	const Context & context,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	const size_t max_block_size,
	const unsigned threads)
{
	size_t result_size = (cluster.getRemoteShardCount() * settings.max_parallel_replicas) + cluster.getLocalShardCount();

	processed_stage = result_size == 1 || settings.distributed_group_by_no_merge
		? QueryProcessingStage::Complete
		: QueryProcessingStage::WithMergeableState;

	const auto & modified_query_ast = rewriteSelectQuery(
		query, remote_database, remote_table);

	Tables external_tables;

	if (settings.global_subqueries_method == GlobalSubqueriesMethod::PUSH)
		external_tables = context.getExternalTables();

	/// Отключаем мультиплексирование шардов, если есть ORDER BY без GROUP BY.
	//const ASTSelectQuery & ast = *(static_cast<const ASTSelectQuery *>(modified_query_ast.get()));

	/** Функциональность shard_multiplexing не доделана - выключаем её.
	  * (Потому что установка соединений с разными шардами в рамках одного потока выполняется не параллельно.)
	  * Подробнее смотрите в https://███████████.yandex-team.ru/METR-18300
	  */
	//bool enable_shard_multiplexing = !(ast.order_expression_list && !ast.group_expression_list);
	bool enable_shard_multiplexing = false;

	ClusterProxy::SelectQueryConstructor select_query_constructor{processed_stage, external_tables};

	return ClusterProxy::Query{select_query_constructor, cluster, modified_query_ast,
		context, settings, enable_shard_multiplexing}.execute();
}

BlockOutputStreamPtr StorageDistributed::write(ASTPtr query, const Settings & settings)
{
	if (!write_enabled)
		throw Exception{
			"Method write is not supported by storage " + getName() +
			" with more than one shard and no sharding key provided",
			ErrorCodes::STORAGE_REQUIRES_PARAMETER
		};

	return std::make_shared<DistributedBlockOutputStream>(
		*this,
		rewriteInsertQuery(query, remote_database, remote_table));
}

void StorageDistributed::alter(const AlterCommands & params, const String & database_name, const String & table_name, const Context & context)
{
	for (const auto & param : params)
		if (param.type == AlterCommand::MODIFY_PRIMARY_KEY)
			throw Exception("Storage engine " + getName() + " doesn't support primary key.", ErrorCodes::NOT_IMPLEMENTED);

	auto lock = lockStructureForAlter();
	params.apply(*columns, materialized_columns, alias_columns, column_defaults);

	context.getDatabase(database_name)->alterTable(
		context, table_name,
		*columns, materialized_columns, alias_columns, column_defaults, {});
}

void StorageDistributed::shutdown()
{
	directory_monitors.clear();
}

void StorageDistributed::reshardPartitions(ASTPtr query, const String & database_name,
	const Field & first_partition, const Field & last_partition,
	const WeightedZooKeeperPaths & weighted_zookeeper_paths,
	const ASTPtr & sharding_key_expr, bool do_copy, const Field & coordinator,
	const Settings & settings)
{
	auto & resharding_worker = context.getReshardingWorker();
	if (!resharding_worker.isStarted())
		throw Exception{"Resharding background thread is not running", ErrorCodes::RESHARDING_NO_WORKER};

	if (!coordinator.isNull())
		throw Exception{"Use of COORDINATE WITH is forbidden in ALTER TABLE ... RESHARD"
			" queries for distributed tables",
			ErrorCodes::RESHARDING_INVALID_PARAMETERS};

	std::string coordinator_id = resharding_worker.createCoordinator(cluster);

	std::atomic<bool> has_notified_error{false};

	std::string dumped_coordinator_state;

	auto handle_exception = [&](const std::string & msg = "")
	{
		try
		{
			if (!has_notified_error)
				resharding_worker.setStatus(coordinator_id, ReshardingWorker::STATUS_ERROR, msg);
			dumped_coordinator_state = resharding_worker.dumpCoordinatorState(coordinator_id);
			resharding_worker.deleteCoordinator(coordinator_id);
		}
		catch (...)
		{
			tryLogCurrentException(__PRETTY_FUNCTION__);
		}
	};

	try
	{
		/// Создать запрос ALTER TABLE ... RESHARD [COPY] PARTITION ... COORDINATE WITH ...

		ASTPtr alter_query_ptr = std::make_shared<ASTAlterQuery>();
		auto & alter_query = static_cast<ASTAlterQuery &>(*alter_query_ptr);

		alter_query.database = remote_database;
		alter_query.table = remote_table;

		alter_query.parameters.emplace_back();
		ASTAlterQuery::Parameters & parameters = alter_query.parameters.back();

		parameters.type = ASTAlterQuery::RESHARD_PARTITION;
		if (!first_partition.isNull())
			parameters.partition = std::make_shared<ASTLiteral>({}, first_partition);
		if (!last_partition.isNull())
			parameters.last_partition = std::make_shared<ASTLiteral>({}, last_partition);

		ASTPtr expr_list = std::make_shared<ASTExpressionList>();
		for (const auto & entry : weighted_zookeeper_paths)
		{
			ASTPtr weighted_path_ptr = std::make_shared<ASTWeightedZooKeeperPath>();
			auto & weighted_path = static_cast<ASTWeightedZooKeeperPath &>(*weighted_path_ptr);
			weighted_path.path = entry.first;
			weighted_path.weight = entry.second;
			expr_list->children.push_back(weighted_path_ptr);
		}

		parameters.weighted_zookeeper_paths = expr_list;
		parameters.sharding_key_expr = sharding_key_expr;
		parameters.do_copy = do_copy;
		parameters.coordinator = std::make_shared<ASTLiteral>({}, coordinator_id);

		resharding_worker.registerQuery(coordinator_id, queryToString(alter_query_ptr));

		/** Функциональность shard_multiplexing не доделана - выключаем её.
		* (Потому что установка соединений с разными шардами в рамках одного потока выполняется не параллельно.)
		* Подробнее смотрите в https://███████████.yandex-team.ru/METR-18300
		*/
		bool enable_shard_multiplexing = false;

		ClusterProxy::AlterQueryConstructor alter_query_constructor;

		BlockInputStreams streams = ClusterProxy::Query{alter_query_constructor, cluster, alter_query_ptr,
			context, settings, enable_shard_multiplexing}.execute();

		/// This callback is called if an exception has occurred while attempting to read
		/// a block from a shard. This is to avoid a potential deadlock if other shards are
		/// waiting inside a barrier. Actually, even without this solution, we would avoid
		/// such a deadlock because we would eventually time out while trying to get remote
		/// blocks. Nevertheless this is not the ideal way of sorting out this issue since
		/// we would then not get to know the actual cause of the failure.
		auto exception_callback = [&resharding_worker, coordinator_id, &has_notified_error]()
		{
			try
			{
				resharding_worker.setStatus(coordinator_id, ReshardingWorker::STATUS_ERROR);
				has_notified_error = true;
			}
			catch (...)
			{
				tryLogCurrentException(__PRETTY_FUNCTION__);
			}
		};

		streams[0] = std::make_shared<UnionBlockInputStream<>>(
			streams, nullptr, settings.max_distributed_connections, exception_callback);
		streams.resize(1);

		auto stream_ptr = dynamic_cast<IProfilingBlockInputStream *>(&*streams[0]);
		if (stream_ptr == nullptr)
			throw Exception{"StorageDistributed: Internal error", ErrorCodes::LOGICAL_ERROR};
		auto & stream = *stream_ptr;

		stream.readPrefix();

		while (!stream.isCancelled() && stream.read())
			;

		if (!stream.isCancelled())
			stream.readSuffix();
	}
	catch (const Exception & ex)
	{
		handle_exception(ex.message());
		LOG_ERROR(log, dumped_coordinator_state);
		throw;
	}
	catch (const std::exception & ex)
	{
		handle_exception(ex.what());
		LOG_ERROR(log, dumped_coordinator_state);
		throw;
	}
	catch (...)
	{
		handle_exception();
		LOG_ERROR(log, dumped_coordinator_state);
		throw;
	}
}

BlockInputStreams StorageDistributed::describe(const Context & context, const Settings & settings)
{
	/// Создать запрос DESCRIBE TABLE.

	ASTPtr describe_query_ptr = std::make_shared<ASTDescribeQuery>();
	auto & describe_query = static_cast<ASTDescribeQuery &>(*describe_query_ptr);

	describe_query.database = remote_database;
	describe_query.table = remote_table;

	/** Функциональность shard_multiplexing не доделана - выключаем её.
	  * (Потому что установка соединений с разными шардами в рамках одного потока выполняется не параллельно.)
	  * Подробнее смотрите в https://███████████.yandex-team.ru/METR-18300
	  */
	bool enable_shard_multiplexing = false;

	ClusterProxy::DescribeQueryConstructor describe_query_constructor;

	return ClusterProxy::Query{describe_query_constructor, cluster, describe_query_ptr,
		context, settings, enable_shard_multiplexing}.execute();
}

NameAndTypePair StorageDistributed::getColumn(const String & column_name) const
{
	if (const auto & type = VirtualColumnFactory::tryGetType(column_name))
		return { column_name, type };

	return getRealColumn(column_name);
}

bool StorageDistributed::hasColumn(const String & column_name) const
{
	return VirtualColumnFactory::hasColumn(column_name) || IStorage::hasColumn(column_name);
}

void StorageDistributed::createDirectoryMonitor(const std::string & name)
{
	directory_monitors.emplace(name, std::make_unique<DirectoryMonitor>(*this, name));
}

void StorageDistributed::createDirectoryMonitors()
{
	if (path.empty())
		return;

	Poco::File{path}.createDirectory();

	Poco::DirectoryIterator end;
	for (Poco::DirectoryIterator it{path}; it != end; ++it)
		if (it->isDirectory())
			createDirectoryMonitor(it.name());
}

void StorageDistributed::requireDirectoryMonitor(const std::string & name)
{
	if (!directory_monitors.count(name))
		createDirectoryMonitor(name);
}

size_t StorageDistributed::getShardCount() const
{
	return cluster.getRemoteShardCount();
}

}
