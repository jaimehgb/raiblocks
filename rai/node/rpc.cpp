#include <rai/node/rpc.hpp>

#include <rai/node/node.hpp>
#include <boost/algorithm/string.hpp>

#include <ed25519-donna/ed25519.h>

rai::rpc_config::rpc_config () :
address (boost::asio::ip::address_v6::loopback ()),
port (rai::rpc::rpc_port),
enable_control (false),
frontier_request_limit (16384),
chain_request_limit (16384)
{
}

rai::rpc_config::rpc_config (bool enable_control_a) :
address (boost::asio::ip::address_v6::loopback ()),
port (rai::rpc::rpc_port),
enable_control (enable_control_a),
frontier_request_limit (16384),
chain_request_limit (16384)
{
}

void rai::rpc_config::serialize_json (boost::property_tree::ptree & tree_a) const
{
    tree_a.put ("address", address.to_string ());
    tree_a.put ("port", std::to_string (port));
    tree_a.put ("enable_control", enable_control);
	tree_a.put ("frontier_request_limit", frontier_request_limit);
	tree_a.put ("chain_request_limit", chain_request_limit);
}

bool rai::rpc_config::deserialize_json (boost::property_tree::ptree const & tree_a)
{
	auto result (false);
    try
    {
		auto address_l (tree_a.get <std::string> ("address"));
		auto port_l (tree_a.get <std::string> ("port"));
		enable_control = tree_a.get <bool> ("enable_control");
		auto frontier_request_limit_l (tree_a.get <std::string> ("frontier_request_limit"));
		auto chain_request_limit_l (tree_a.get <std::string> ("chain_request_limit"));
		try
		{
			port = std::stoul (port_l);
			result = port > std::numeric_limits <uint16_t>::max ();
			frontier_request_limit = std::stoull (frontier_request_limit_l);
			chain_request_limit = std::stoull (chain_request_limit_l);
		}
		catch (std::logic_error const &)
		{
			result = true;
		}
		boost::system::error_code ec;
		address = boost::asio::ip::address_v6::from_string (address_l, ec);
		if (ec)
		{
			result = true;
		}
    }
    catch (std::runtime_error const &)
    {
        result = true;
    }
	return result;
}

rai::rpc::rpc (boost::asio::io_service & service_a, rai::node & node_a, rai::rpc_config const & config_a) :
acceptor (service_a),
config (config_a),
node (node_a)
{
	auto endpoint (rai::tcp_endpoint (config_a.address, config_a.port));
	acceptor.open (endpoint.protocol ());
    acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true));
	acceptor.bind (endpoint);
	acceptor.listen ();
	node_a.observers.blocks.add ([this] (rai::block const & block_a, rai::account const & account_a, rai::amount const &)
	{
		observer_action (account_a);
	});
}

void rai::rpc::start ()
{
	auto connection (std::make_shared <rai::rpc_connection> (node, *this));
	acceptor.async_accept (connection->socket, [this, connection] (boost::system::error_code const & ec)
	{
		if (!ec)
		{
			start ();
			connection->parse_connection ();
		}
		else
		{
			BOOST_LOG (this->node.log) << boost::str (boost::format ("Error accepting RPC connections: %1%") % ec);
		}
	});
}

void rai::rpc::stop ()
{
	acceptor.close ();
}

rai::rpc_handler::rpc_handler (rai::node & node_a, rai::rpc & rpc_a, std::string const & body_a, std::function <void (boost::property_tree::ptree const &)> const & response_a) :
body (body_a),
node (node_a),
rpc (rpc_a),
response (response_a)
{
}

void rai::rpc::observer_action (rai::account const & account_a)
{
	std::shared_ptr <rai::payment_observer> observer;
	{
		std::lock_guard <std::mutex> lock (mutex);
		auto existing (payment_observers.find (account_a));
		if (existing != payment_observers.end ())
		{
			observer = existing->second;
		}
	}
	if (observer != nullptr)
	{
		observer->observe ();
	}
}

namespace
{
void error_response (std::function <void (boost::property_tree::ptree const &)> response_a, std::string const & message_a)
{
	boost::property_tree::ptree response_l;
	response_l.put ("error", message_a);
	response_a (response_l);
}

bool decode_unsigned (std::string const & text, uint64_t & number)
{
	bool result;
	size_t end;
	try
	{
		number = std::stoull (text, &end);
		result = false;
	}
	catch (std::invalid_argument const &)
	{
		result = true;
	}
	catch (std::out_of_range const &)
	{
		result = true;
	}
	result = result || end != text.size ();
	return result;
}
}

void rai::rpc_handler::account_balance ()
{
	std::string account_text (request.get <std::string> ("account"));
	rai::uint256_union account;
	auto error (account.decode_account (account_text));
	if (!error)
	{
		auto balance (node.balance_pending (account));
		boost::property_tree::ptree response_l;
		response_l.put ("balance", balance.first.convert_to <std::string> ());
		response_l.put ("pending", balance.second.convert_to <std::string> ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::account_block_count ()
{
	std::string account_text (request.get <std::string> ("account"));
	rai::uint256_union account;
	auto error (account.decode_account (account_text));
	if (!error)
	{
		rai::transaction transaction (node.store.environment, nullptr, false);
		rai::account_info info;
		if (!node.store.account_get (transaction, account, info))
		{
			boost::property_tree::ptree response_l;
			response_l.put ("block_count", std::to_string (info.block_count));
			response (response_l);
		}
		else
		{
			error_response (response, "Account not found");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::account_create ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				rai::account new_key (existing->second->deterministic_insert ());
				if (!new_key.is_zero ())
				{
					boost::property_tree::ptree response_l;
					response_l.put ("account", new_key.to_account ());
					response (response_l);
				}
				else
				{
					error_response (response, "Wallet is locked");
				}
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad wallet number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::account_get ()
{
	std::string key_text (request.get <std::string> ("key"));
	rai::uint256_union pub;
	auto error (pub.decode_hex (key_text));
	if (!error)
	{
		boost::property_tree::ptree response_l;
		response_l.put ("account", pub.to_account ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad public key");
	}
}

void rai::rpc_handler::account_key ()
{
	std::string account_text (request.get <std::string> ("account"));
	rai::account account;
	auto error (account.decode_account (account_text));
	if (!error)
	{
		boost::property_tree::ptree response_l;
		response_l.put ("key", account.to_string ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::account_list ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree accounts;
			rai::transaction transaction (node.store.environment, nullptr, false);
			for (auto i (existing->second->store.begin (transaction)), j (existing->second->store.end ()); i != j; ++i)
			{
				boost::property_tree::ptree entry;
				entry.put ("", rai::uint256_union (i->first).to_account ());
				accounts.push_back (std::make_pair ("", entry));
			}
			response_l.add_child ("accounts", accounts);
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad wallet number");
	}
}

void rai::rpc_handler::account_move ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		std::string source_text (request.get <std::string> ("source"));
		auto accounts_text (request.get_child ("accounts"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				auto wallet (existing->second);
				rai::uint256_union source;
				auto error (source.decode_hex (source_text));
				if (!error)
				{
					auto existing (node.wallets.items.find (source));
					if (existing != node.wallets.items.end ())
					{
						auto source (existing->second);
						std::vector <rai::public_key> accounts;
						for (auto i (accounts_text.begin ()), n (accounts_text.end ()); i != n; ++i)
						{
							rai::public_key account;
							account.decode_hex (i->second.get <std::string> (""));
							accounts.push_back (account);
						}
						rai::transaction transaction (node.store.environment, nullptr, true);
						auto error (wallet->store.move (transaction, source->store, accounts));
						boost::property_tree::ptree response_l;
						response_l.put ("moved", error ? "0" : "1");
						response (response_l);
					}
					else
					{
						error_response (response, "Source not found");
					}
				}
				else
				{
					error_response (response, "Bad source number");
				}
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad wallet number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::account_remove ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		std::string account_text (request.get <std::string> ("account"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				auto wallet (existing->second);
				rai::transaction transaction (node.store.environment, nullptr, true);
				if (existing->second->store.valid_password (transaction))
				{
					rai::account account_id;
					auto error (account_id.decode_account (account_text));
					if (!error)
					{
						auto account (wallet->store.find (transaction, account_id));
						if (account != wallet->store.end ())
						{
							wallet->store.erase (transaction, account_id);
							boost::property_tree::ptree response_l;
							response_l.put ("removed", "1");
							response (response_l);
						}
						else
						{
							error_response (response, "Account not found in wallet");
						}
					}
					else
					{
						error_response (response, "Bad account number");
					}
				}
				else
				{
					error_response (response, "Wallet locked");
				}
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad wallet number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::account_representative ()
{
	std::string account_text (request.get <std::string> ("account"));
	rai::account account;
	auto error (account.decode_account (account_text));
	if (!error)
	{
		rai::transaction transaction (node.store.environment, nullptr, false);
		rai::account_info info;
		auto error (node.store.account_get (transaction, account, info));
		if (!error)
		{
			auto block (node.store.block_get (transaction, info.rep_block));
			assert (block != nullptr);
			boost::property_tree::ptree response_l;
			response_l.put ("representative", block->representative ().to_account ());
			response (response_l);
		}
		else
		{
			error_response (response, "Account not found");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::account_representative_set ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				auto wallet (existing->second);
				std::string account_text (request.get <std::string> ("account"));
				rai::account account;
				auto error (account.decode_account (account_text));
				if (!error)
				{
					std::string representative_text (request.get <std::string> ("representative"));
					rai::account representative;
					auto error (representative.decode_account (representative_text));
					if (!error)
					{
						auto response_a (response);
						wallet->change_async (account, representative, [response_a] (std::unique_ptr <rai::block> block)
						{
							rai::block_hash hash (0);
							if (block != nullptr)
							{
								hash = block->hash ();
							}
							boost::property_tree::ptree response_l;
							response_l.put ("block", hash.to_string ());
							response_a (response_l);
						});
					}
				}
				else
				{
					error_response (response, "Bad account number");
				}
			}
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::account_weight ()
{
	std::string account_text (request.get <std::string> ("account"));
	rai::uint256_union account;
	auto error (account.decode_account (account_text));
	if (!error)
	{
		auto balance (node.weight (account));
		boost::property_tree::ptree response_l;
		response_l.put ("weight", balance.convert_to <std::string> ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::accounts_balances ()
{
	boost::property_tree::ptree response_l;
	boost::property_tree::ptree balances;
	for (auto &accounts : request.get_child ("accounts"))
	{
		std::string account_text = accounts.second.data ();
		rai::uint256_union account;
		auto error (account.decode_account (account_text));
		if (!error)
		{
			boost::property_tree::ptree entry;
			auto balance (node.balance_pending (account));
			entry.put ("balance", balance.first.convert_to <std::string> ());
			entry.put ("pending", balance.second.convert_to <std::string> ());
			balances.push_back (std::make_pair (account.to_account (), entry));
		}
		else
		{
			error_response (response, "Bad account number");
		}
	}
	response_l.add_child ("balances", balances);
	response (response_l);
}


void rai::rpc_handler::accounts_frontiers ()
{
	boost::property_tree::ptree response_l;
	boost::property_tree::ptree frontiers;
	rai::transaction transaction (node.store.environment, nullptr, false);
	for (auto &accounts : request.get_child("accounts"))
	{
		std::string account_text = accounts.second.data ();
		rai::uint256_union account;
		auto error (account.decode_account (account_text));
		if (!error)
		{
			auto latest (node.ledger.latest (transaction, account));
			if (!latest.is_zero ())
			{
				frontiers.put (account.to_account (), latest.to_string ());
			}
		}
		else
		{
			error_response (response, "Bad account number");
		}
	}
	response_l.add_child ("frontiers", frontiers);
	response (response_l);
}

void rai::rpc_handler::accounts_pending ()
{
	std::string count_text (request.get <std::string> ("count"));
	uint64_t count;
	if (!decode_unsigned (count_text, count))
	{
		boost::property_tree::ptree response_l;
		boost::property_tree::ptree pending;
		rai::transaction transaction (node.store.environment, nullptr, false);
		for (auto &accounts : request.get_child("accounts"))
		{
			std::string account_text = accounts.second.data ();
			rai::uint256_union account;
			if (!account.decode_account (account_text))
			{
				boost::property_tree::ptree peers_l;
				rai::account end (account.number () + 1);
				for (auto i (node.store.pending_begin (transaction, rai::pending_key (account, 0))), n (node.store.pending_begin (transaction, rai::pending_key (end, 0))); i != n && peers_l.size () < count; ++i)
				{
					rai::pending_key key (i->first);
					boost::property_tree::ptree entry;
					entry.put ("", key.hash.to_string ());
					peers_l.push_back (std::make_pair ("", entry));
				}
				pending.add_child (account.to_account (), peers_l);
			}
			else
			{
				error_response (response, "Bad account number");
			}
		}
		response_l.add_child ("blocks", pending);
		response (response_l);
	}
	else
	{
		error_response (response, "Invalid count");
	}
}

void rai::rpc_handler::available_supply ()
{
	auto genesis_balance (node.balance (rai::genesis_account)); // Cold storage genesis
	auto landing_balance (node.balance (rai::account ("059F68AAB29DE0D3A27443625C7EA9CDDB6517A8B76FE37727EF6A4D76832AD5"))); // Active unavailable account
	auto faucet_balance (node.balance (rai::account ("8E319CE6F3025E5B2DF66DA7AB1467FE48F1679C13DD43BFDB29FA2E9FC40D3B"))); // Faucet account
	auto available (rai::genesis_amount - genesis_balance - landing_balance - faucet_balance);
	boost::property_tree::ptree response_l;
	response_l.put ("available", available.convert_to <std::string> ());
	response (response_l);
}

void rai::rpc_handler::block ()
{
	std::string hash_text (request.get <std::string> ("hash"));
	rai::uint256_union hash;
	auto error (hash.decode_hex (hash_text));
	if (!error)
	{
		rai::transaction transaction (node.store.environment, nullptr, false);
		auto block (node.store.block_get (transaction, hash));
		if (block != nullptr)
		{
			boost::property_tree::ptree response_l;
			std::string contents;
			block->serialize_json (contents);
			response_l.put ("contents", contents);
			response (response_l);
		}
		else
		{
			error_response (response, "Block not found");
		}
	}
	else
	{
		error_response (response, "Bad hash number");
	}
}

void rai::rpc_handler::blocks ()
{
	std::vector <std::string> hashes;
	boost::property_tree::ptree response_l;
	boost::property_tree::ptree blocks;
	rai::transaction transaction (node.store.environment, nullptr, false);
	for (boost::property_tree::ptree::value_type &hashes : request.get_child("hashes"))
	{
		std::string hash_text = hashes.second.data();
		rai::uint256_union hash;
		auto error (hash.decode_hex (hash_text));
		if (!error)
		{
			auto block (node.store.block_get (transaction, hash));
			if (block != nullptr)
			{
				std::string contents;
				block->serialize_json (contents);
				blocks.put (hash_text, contents);
			}
			else
			{
				error_response (response, "Block not found");
			}
		}
		else
		{
			error_response (response, "Bad hash number");
		}
	}
	response_l.add_child ("blocks", blocks);
	response (response_l);
}

void rai::rpc_handler::block_account ()
{
	std::string hash_text (request.get <std::string> ("hash"));
	rai::block_hash hash;
	if (!hash.decode_hex (hash_text))
	{
		rai::transaction transaction (node.store.environment, nullptr, false);
		if (node.store.block_exists (transaction, hash))
		{
			boost::property_tree::ptree response_l;
			auto account (node.ledger.account (transaction, hash));
			response_l.put ("account", account.to_account ());
			response (response_l);
		}
		else
		{
			error_response (response, "Block not found");
		}
	}
	else
	{
		error_response (response, "Invalid block hash");
	}
}

void rai::rpc_handler::block_count ()
{
	rai::transaction transaction (node.store.environment, nullptr, false);
	boost::property_tree::ptree response_l;
	response_l.put ("count", std::to_string (node.store.block_count (transaction).sum ()));
	response_l.put ("unchecked", std::to_string (node.store.unchecked_count (transaction)));
	response (response_l);
}

void rai::rpc_handler::successors ()
{
	std::string block_text (request.get <std::string> ("block"));
	std::string count_text (request.get <std::string> ("count"));
	rai::block_hash block;
	if (!block.decode_hex (block_text))
	{
		uint64_t count;
		if (!decode_unsigned (count_text, count))
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree blocks;
			rai::transaction transaction (node.store.environment, nullptr, false);
			while (!block.is_zero () && blocks.size () < count)
			{
				auto block_l (node.store.block_get (transaction, block));
				if (block_l != nullptr)
				{
					boost::property_tree::ptree entry;
					entry.put ("", block.to_string ());
					blocks.push_back (std::make_pair ("", entry));
					block = node.store.block_successor (transaction, block);
				}
				else
				{
					block.clear ();
				}
			}
			response_l.add_child ("blocks", blocks);
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid count limit");
		}
	}
	else
	{
		error_response (response, "Invalid block hash");
	}
}

void rai::rpc_handler::bootstrap ()
{
	std::string address_text = request.get <std::string> ("address");
	std::string port_text = request.get <std::string> ("port");
	boost::system::error_code ec;
	auto address (boost::asio::ip::address_v6::from_string (address_text, ec));
	if (!ec)
	{
		uint16_t port;
		if (!rai::parse_port (port_text, port))
		{
			node.bootstrap_initiator.bootstrap (rai::endpoint (address, port));
			boost::property_tree::ptree response_l;
			response_l.put ("success", "");
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid port");
		}
	}
	else
	{
		error_response (response, "Invalid address");
	}
}

void rai::rpc_handler::bootstrap_any ()
{
	node.bootstrap_initiator.bootstrap ();
	boost::property_tree::ptree response_l;
	response_l.put ("success", "");
	response (response_l);
}

void rai::rpc_handler::chain ()
{
	std::string block_text (request.get <std::string> ("block"));
	std::string count_text (request.get <std::string> ("count"));
	rai::block_hash block;
	if (!block.decode_hex (block_text))
	{
		uint64_t count;
		if (!decode_unsigned (count_text, count))
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree blocks;
			rai::transaction transaction (node.store.environment, nullptr, false);
			while (!block.is_zero () && blocks.size () < count)
			{
				auto block_l (node.store.block_get (transaction, block));
				if (block_l != nullptr)
				{
					boost::property_tree::ptree entry;
					entry.put ("", block.to_string ());
					blocks.push_back (std::make_pair ("", entry));
					block = block_l->previous ();
				}
				else
				{
					block.clear ();
				}
			}
			response_l.add_child ("blocks", blocks);
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid count limit");
		}
	}
	else
	{
		error_response (response, "Invalid block hash");
	}
}

void rai::rpc_handler::deterministic_key ()
{
	std::string seed_text (request.get <std::string> ("seed"));
	std::string index_text (request.get <std::string> ("index"));
	rai::raw_key seed;
	auto error (seed.data.decode_hex (seed_text));
	if (!error)
	{
		uint64_t index_a;
		if (!decode_unsigned (index_text, index_a))
		{
			rai::uint256_union index (index_a);
			rai::uint256_union prv;
			blake2b_state hash;
			blake2b_init (&hash, prv.bytes.size ());
			blake2b_update (&hash, seed.data.bytes.data (), seed.data.bytes.size ());
			blake2b_update (&hash, reinterpret_cast <uint8_t *> (&index.dwords [7]), sizeof (uint32_t));
			blake2b_final (&hash, prv.bytes.data (), prv.bytes.size ());
			boost::property_tree::ptree response_l;
			rai::uint256_union pub;
			ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
			response_l.put ("private", prv.to_string ());
			response_l.put ("public", pub.to_string ());
			response_l.put ("account", pub.to_account ());
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid index");
		}
	}
	else
	{
		error_response (response, "Bad seed");
	}
}

void rai::rpc_handler::frontiers ()
{
	std::string account_text (request.get <std::string> ("account"));
	std::string count_text (request.get <std::string> ("count"));
	rai::account start;
	if (!start.decode_account (account_text))
	{
		uint64_t count;
		if (!decode_unsigned (count_text, count))
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree frontiers;
			rai::transaction transaction (node.store.environment, nullptr, false);
			for (auto i (node.store.latest_begin (transaction, start)), n (node.store.latest_end ()); i != n && frontiers.size () < count; ++i)
			{
				frontiers.put (rai::account (i->first).to_account (), rai::account_info (i->second).head.to_string ());
			}
			response_l.add_child ("frontiers", frontiers);
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid count limit");
		}
	}
	else
	{
		error_response (response, "Invalid starting account");
	}
}

void rai::rpc_handler::frontier_count ()
{
	rai::transaction transaction (node.store.environment, nullptr, false);
	auto size (node.store.frontier_count (transaction));
	boost::property_tree::ptree response_l;
	response_l.put ("count", std::to_string (size));
	response (response_l);
}

namespace
{
class history_visitor : public rai::block_visitor
{
public:
	history_visitor (rai::rpc_handler & handler_a, rai::transaction & transaction_a, boost::property_tree::ptree & tree_a, rai::block_hash const & hash_a) :
	handler (handler_a),
	transaction (transaction_a),
	tree (tree_a),
	hash (hash_a)
	{
	}
	void send_block (rai::send_block const & block_a)
	{
		tree.put ("type", "send");
		auto account (block_a.hashables.destination.to_account ());
		tree.put ("account", account);
		auto amount (handler.node.ledger.amount (transaction, hash).convert_to <std::string> ());
		tree.put ("amount", amount);
	}
	void receive_block (rai::receive_block const & block_a)
	{
		tree.put ("type", "receive");
		auto account (handler.node.ledger.account (transaction, block_a.hashables.source).to_account ());
		tree.put ("account", account);
		auto amount (handler.node.ledger.amount (transaction, hash).convert_to <std::string> ());
		tree.put ("amount", amount);
	}
	void open_block (rai::open_block const & block_a)
	{
		// Report opens as a receive
		tree.put ("type", "receive");
		if (block_a.hashables.source != rai::genesis_account)
		{
			tree.put ("account", handler.node.ledger.account (transaction, block_a.hashables.source).to_account ());
			tree.put ("amount", handler.node.ledger.amount (transaction, hash).convert_to <std::string> ());
		}
		else
		{
			tree.put ("account", rai::genesis_account.to_account ());
			tree.put ("amount", rai::genesis_amount.convert_to <std::string> ());
		}
	}
	void change_block (rai::change_block const &)
	{
		// Don't report change blocks
	}
	rai::rpc_handler & handler;
	rai::transaction & transaction;
	boost::property_tree::ptree & tree;
	rai::block_hash const & hash;
};
}

void rai::rpc_handler::history ()
{
	std::string hash_text (request.get <std::string> ("hash"));
	std::string count_text (request.get <std::string> ("count"));
	rai::block_hash hash;
	if (!hash.decode_hex (hash_text))
	{
		uint64_t count;
		if (!decode_unsigned (count_text, count))
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree history;
			rai::transaction transaction (node.store.environment, nullptr, false);
			auto block (node.store.block_get (transaction, hash));
			while (block != nullptr && count > 0)
			{
				boost::property_tree::ptree entry;
				history_visitor visitor (*this, transaction, entry, hash);
				block->visit (visitor);
				if (!entry.empty ())
				{
					entry.put ("hash", hash.to_string ());
					history.push_back (std::make_pair ("", entry));
				}
				hash = block->previous ();
				block = node.store.block_get (transaction, hash);
				--count;
			}
			response_l.add_child ("history", history);
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid count limit");
		}
	}
	else
	{
		error_response (response, "Invalid block hash");
	}
}

void rai::rpc_handler::account_history ()
{
	std::string account_text (request.get <std::string> ("account"));
	std::string count_text (request.get <std::string> ("count"));
	rai::uint256_union account;
	auto error (account.decode_account (account_text));
	if (!error)
	{
		uint64_t count;
		if (!decode_unsigned (count_text, count))
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree history;
			rai::transaction transaction (node.store.environment, nullptr, false);
			auto hash (node.ledger.latest (transaction, account));
			auto block (node.store.block_get (transaction, hash));
			while (block != nullptr && count > 0)
			{
				boost::property_tree::ptree entry;
				history_visitor visitor (*this, transaction, entry, hash);
				block->visit (visitor);
				if (!entry.empty ())
				{
					entry.put ("hash", hash.to_string ());
					history.push_back (std::make_pair ("", entry));
				}
				hash = block->previous ();
				block = node.store.block_get (transaction, hash);
				--count;
			}
			response_l.add_child ("history", history);
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid count limit");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::keepalive ()
{
	if (rpc.config.enable_control)
	{
		std::string address_text (request.get <std::string> ("address"));
		std::string port_text (request.get <std::string> ("port"));
		uint16_t port;
		if (!rai::parse_port (port_text, port))
		{
			node.keepalive (address_text, port);
			boost::property_tree::ptree response_l;
			response (response_l);
		}
		else
		{
			error_response (response, "Invalid port");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::key_create ()
{
	boost::property_tree::ptree response_l;
	rai::keypair pair;
	response_l.put ("private", pair.prv.data.to_string ());
	response_l.put ("public", pair.pub.to_string ());
	response_l.put ("account", pair.pub.to_account ());
	response (response_l);
}

void rai::rpc_handler::key_expand ()
{
	std::string key_text (request.get <std::string> ("key"));
	rai::uint256_union prv;
	auto error (prv.decode_hex (key_text));
	if (!error)
	{
		boost::property_tree::ptree response_l;
		rai::uint256_union pub;
		ed25519_publickey (prv.bytes.data (), pub.bytes.data ());
		response_l.put ("private", prv.to_string ());
		response_l.put ("public", pub.to_string ());
		response_l.put ("account", pub.to_account ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad private key");
	}
}

void rai::rpc_handler::mrai_from_raw ()
{
	std::string amount_text (request.get <std::string> ("amount"));
	rai::uint128_union amount;
	if (!amount.decode_dec (amount_text))
	{
		auto result (amount.number () / rai::Mxrb_ratio);
		boost::property_tree::ptree response_l;
		response_l.put ("amount", result.convert_to <std::string> ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad amount number");
	}
}

void rai::rpc_handler::mrai_to_raw ()
{
	std::string amount_text (request.get <std::string> ("amount"));
	rai::uint128_union amount;
	if (!amount.decode_dec (amount_text))
	{
		auto result (amount.number () * rai::Mxrb_ratio);
		if (result > amount.number ())
		{
			boost::property_tree::ptree response_l;
			response_l.put ("amount", result.convert_to <std::string> ());
			response (response_l);
		}
		else
		{
			error_response (response, "Amount too big");
		}
	}
	else
	{
		error_response (response, "Bad amount number");
	}
}

void rai::rpc_handler::krai_from_raw ()
{
	std::string amount_text (request.get <std::string> ("amount"));
	rai::uint128_union amount;
	if (!amount.decode_dec (amount_text))
	{
		auto result (amount.number () / rai::kxrb_ratio);
		boost::property_tree::ptree response_l;
		response_l.put ("amount", result.convert_to <std::string> ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad amount number");
	}
}

void rai::rpc_handler::krai_to_raw ()
{
	std::string amount_text (request.get <std::string> ("amount"));
	rai::uint128_union amount;
	if (!amount.decode_dec (amount_text))
	{
		auto result (amount.number () * rai::kxrb_ratio);
		if (result > amount.number ())
		{
			boost::property_tree::ptree response_l;
			response_l.put ("amount", result.convert_to <std::string> ());
			response (response_l);
		}
		else
		{
			error_response (response, "Amount too big");
		}
	}
	else
	{
		error_response (response, "Bad amount number");
	}
}

void rai::rpc_handler::password_change ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				rai::transaction transaction (node.store.environment, nullptr, true);
				boost::property_tree::ptree response_l;
				std::string password_text (request.get <std::string> ("password"));
				auto error (existing->second->store.rekey (transaction, password_text));
				response_l.put ("changed", error ? "0" : "1");
				response (response_l);
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad account number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::password_enter ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			boost::property_tree::ptree response_l;
			std::string password_text (request.get <std::string> ("password"));
			auto error (existing->second->enter_password (password_text));
			response_l.put ("valid", error ? "0" : "1");
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::password_valid ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			rai::transaction transaction (node.store.environment, nullptr, false);
			boost::property_tree::ptree response_l;
			response_l.put ("valid", existing->second->store.valid_password (transaction) ? "1" : "0");
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::peers ()
{
	boost::property_tree::ptree response_l;
	boost::property_tree::ptree peers_l;
	auto peers_list (node.peers.list());
	for (auto i (peers_list.begin ()), n (peers_list.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		std::stringstream text;
		text << *i;
		entry.put ("", text.str ());
		peers_l.push_back (std::make_pair ("", entry));
	}
	response_l.add_child ("peers", peers_l);
	response (response_l);
}

void rai::rpc_handler::pending ()
{
	std::string account_text (request.get <std::string> ("account"));
	rai::account account;
	if (!account.decode_account(account_text))
	{
		std::string count_text (request.get <std::string> ("count"));
		uint64_t count;
		if (!decode_unsigned (count_text, count))
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree peers_l;
			{
				rai::transaction transaction (node.store.environment, nullptr, false);
				rai::account end (account.number () + 1);
				for (auto i (node.store.pending_begin (transaction, rai::pending_key (account, 0))), n (node.store.pending_begin (transaction, rai::pending_key (end, 0))); i != n && peers_l.size ()< count; ++i)
				{
					rai::pending_key key (i->first);
					boost::property_tree::ptree entry;
					entry.put ("", key.hash.to_string ());
					peers_l.push_back (std::make_pair ("", entry));
				}
			}
			response_l.add_child ("blocks", peers_l);
			response (response_l);
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::payment_begin ()
{
	std::string id_text (request.get <std::string> ("wallet"));
	rai::uint256_union id;
	if (!id.decode_hex (id_text))
	{
		auto existing (node.wallets.items.find (id));
		if (existing != node.wallets.items.end ())
		{
			rai::transaction transaction (node.store.environment, nullptr, true);
			std::shared_ptr <rai::wallet> wallet (existing->second);
			if (wallet->store.valid_password (transaction))
			{
				rai::account account (0);
				do
				{
					auto existing (wallet->free_accounts.begin ());
					if (existing != wallet->free_accounts.end ())
					{
						account = *existing;
						wallet->free_accounts.erase (existing);
						if (wallet->store.find (transaction, account) == wallet->store.end ())
						{
							BOOST_LOG (node.log) << boost::str (boost::format ("Transaction wallet %1% externally modified listing account %1% as free but no longer exists") % id.to_string () % account.to_account ());
							account.clear ();
						}
						else
						{
							if (!node.ledger.account_balance (transaction, account).is_zero ())
							{
								BOOST_LOG (node.log) << boost::str (boost::format ("Skipping account %1% for use as a transaction account since it's balance isn't zero") % account.to_account ());
								account.clear ();
							}
						}
					}
					else
					{
						account = wallet->deterministic_insert (transaction);
						break;
					}
				} while (account.is_zero ());
				if (!account.is_zero ())
				{
					boost::property_tree::ptree response_l;
					response_l.put ("account", account.to_account ());
					response (response_l);
				}
				else
				{
					error_response (response, "Unable to create transaction account");
				}
			}
			else
			{
				error_response (response, "Wallet locked");
			}
		}
		else
		{
			error_response (response, "Unable to find wallets");
		}
	}
	else
	{
		error_response (response, "Bad wallet number");
	}
}

void rai::rpc_handler::payment_init ()
{
	std::string id_text (request.get <std::string> ("wallet"));
	rai::uint256_union id;
	if (!id.decode_hex (id_text))
	{
		rai::transaction transaction (node.store.environment, nullptr, true);
		auto existing (node.wallets.items.find (id));
		if (existing != node.wallets.items.end ())
		{
			auto wallet (existing->second);
			if (wallet->store.valid_password (transaction))
			{
				wallet->init_free_accounts (transaction);
				boost::property_tree::ptree response_l;
				response_l.put ("status", "Ready");
				response (response_l);
			}
			else
			{
				boost::property_tree::ptree response_l;
				response_l.put ("status", "Transaction wallet locked");
				response (response_l);
			}
		}
		else
		{
			boost::property_tree::ptree response_l;
			response_l.put ("status", "Unable to find transaction wallet");
			response (response_l);
		}
	}
	else
	{
		error_response (response, "Bad transaction wallet number");
	}
}

void rai::rpc_handler::payment_end ()
{
	std::string id_text (request.get <std::string> ("wallet"));
	std::string account_text (request.get <std::string> ("account"));
	rai::uint256_union id;
	if (!id.decode_hex (id_text))
	{
		rai::transaction transaction (node.store.environment, nullptr, false);
		auto existing (node.wallets.items.find (id));
		if (existing != node.wallets.items.end ())
		{
			auto wallet (existing->second);
			rai::account account;
			if (!account.decode_account (account_text))
			{
				auto existing (wallet->store.find (transaction, account));
				if (existing != wallet->store.end ())
				{
					if (node.ledger.account_balance (transaction, account).is_zero ())
					{
						wallet->free_accounts.insert (account);
						boost::property_tree::ptree response_l;
						response (response_l);
					}
					else
					{
						error_response (response, "Account has non-zero balance");
					}
				}
				else
				{
					error_response (response, "Account not in wallet");
				}
			}
			else
			{
				error_response (response, "Invalid account number");
			}
		}
		else
		{
			error_response (response, "Unable to find wallet");
		}
	}
	else
	{
		error_response (response, "Bad wallet number");
	}
}

void rai::rpc_handler::payment_wait ()
{
	std::string account_text (request.get <std::string> ("account"));
	std::string amount_text (request.get <std::string> ("amount"));
	std::string timeout_text (request.get <std::string> ("timeout"));
	rai::uint256_union account;
	if (!account.decode_account (account_text))
	{
		rai::uint128_union amount;
		if (!amount.decode_dec (amount_text))
		{
			uint64_t timeout;
			if (!decode_unsigned (timeout_text, timeout))
			{
				{
					auto observer (std::make_shared <rai::payment_observer> (response, rpc, account, amount));
					observer->start (timeout);
					std::lock_guard <std::mutex> lock (rpc.mutex);
					assert (rpc.payment_observers.find (account) == rpc.payment_observers.end ());
					rpc.payment_observers [account] = observer;
				}
				rpc.observer_action (account);
			}
			else
			{
				error_response (response, "Bad timeout number");
			}
		}
		else
		{
			error_response (response, "Bad amount number");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::process ()
{
	std::string block_text (request.get <std::string> ("block"));
	boost::property_tree::ptree block_l;
	std::stringstream block_stream (block_text);
	boost::property_tree::read_json (block_stream, block_l);
	auto block (rai::deserialize_block_json (block_l));
	if (block != nullptr)
	{
		if (!node.work.work_validate (*block))
		{
			node.process_receive_republish (std::move (block));
			boost::property_tree::ptree response_l;
			response (response_l);
		}
		else
		{
			error_response (response, "Block work is invalid");
		}
	}
	else
	{
		error_response (response, "Block is invalid");
	}
}

void rai::rpc_handler::rai_from_raw ()
{
	std::string amount_text (request.get <std::string> ("amount"));
	rai::uint128_union amount;
	if (!amount.decode_dec (amount_text))
	{
		auto result (amount.number () / rai::xrb_ratio);
		boost::property_tree::ptree response_l;
		response_l.put ("amount", result.convert_to <std::string> ());
		response (response_l);
	}
	else
	{
		error_response (response, "Bad amount number");
	}
}

void rai::rpc_handler::rai_to_raw ()
{
	std::string amount_text (request.get <std::string> ("amount"));
	rai::uint128_union amount;
	if (!amount.decode_dec (amount_text))
	{
		auto result (amount.number () * rai::xrb_ratio);
		if (result > amount.number ())
		{
			boost::property_tree::ptree response_l;
			response_l.put ("amount", result.convert_to <std::string> ());
			response (response_l);
		}
		else
		{
			error_response (response, "Amount too big");
		}
	}
	else
	{
		error_response (response, "Bad amount number");
	}
}

void rai::rpc_handler::receive ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				std::string account_text (request.get <std::string> ("account"));
				rai::account account;
				auto error (account.decode_account (account_text));
				if (!error)
				{
					rai::transaction transaction (node.store.environment, nullptr, false);
					auto account_check (existing->second->store.find (transaction, account));
					if (account_check != existing->second->store.end ())
					{
						std::string hash_text (request.get <std::string> ("block"));
						rai::uint256_union hash;
						auto error (hash.decode_hex (hash_text));
						if (!error)
						{
							auto block (node.store.block_get (transaction, hash));
							if (block != nullptr)
							{
								if (node.store.pending_exists (transaction, rai::pending_key (account, hash)))
								{
									auto response_a (response);
									existing->second->receive_async (static_cast <rai::send_block &>(*block), account, rai::genesis_amount, [response_a] (std::unique_ptr <rai::block> block_a)
									{
										rai::uint256_union hash_a (0);
										if (block_a != nullptr)
										{
											hash_a = block_a->hash ();
										}
										boost::property_tree::ptree response_l;
										response_l.put ("block", hash_a.to_string ());
										response_a (response_l);
									});
								}
								else
								{
									error_response (response, "Block is not available to receive");
								}
							}
							else
							{
								error_response (response, "Block not found");
							}
						}
						else
						{
							error_response (response, "Bad block number");
						}
					}
					else
					{
						error_response (response, "Account not found in wallet");
					}
				}
				else
				{
					error_response (response, "Bad account number");
				}
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad wallet number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::representatives ()
{
	boost::property_tree::ptree response_l;
	boost::property_tree::ptree representatives;
	rai::transaction transaction (node.store.environment, nullptr, false);
	for (auto i (node.store.representation_begin (transaction)), n (node.store.representation_end ()); i != n; ++i)
	{
		rai::account account(i->first);
		auto amount (node.store.representation_get (transaction, account));
		representatives.put (account.to_account (), amount.convert_to <std::string> ());
	}
	response_l.add_child ("representatives", representatives);
	response (response_l);
}

void rai::rpc_handler::republish ()
{
	std::string hash_text (request.get <std::string> ("hash"));
	rai::uint256_union hash;
	auto error (hash.decode_hex (hash_text));
	if (!error)
	{
		rai::transaction transaction (node.store.environment, nullptr, false);
		auto block (node.store.block_get (transaction, hash));
		if (block != nullptr)
		{
			while (!hash.is_zero ())
			{
				block = node.store.block_get (transaction, hash);
				node.network.republish_block (*block);
				hash = node.store.block_successor (transaction, hash);
			}
			boost::property_tree::ptree response_l;
			response_l.put ("success", "");
			response (response_l);
		}
		else
		{
			error_response (response, "Block not found");
		}
	}
	else
	{
		error_response (response, "Bad hash number");
	}
}

void rai::rpc_handler::search_pending ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				auto error (existing->second->search_pending ());
				boost::property_tree::ptree response_l;
				response_l.put ("started", !error);
				response (response_l);
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::send ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				std::string source_text (request.get <std::string> ("source"));
				rai::account source;
				auto error (source.decode_account (source_text));
				if (!error)
				{
					std::string destination_text (request.get <std::string> ("destination"));
					rai::account destination;
					auto error (destination.decode_account (destination_text));
					if (!error)
					{
						std::string amount_text (request.get <std::string> ("amount"));
						rai::amount amount;
						auto error (amount.decode_dec (amount_text));
						if (!error)
						{
							auto rpc_l (shared_from_this ());
							auto response_a (response);
							existing->second->send_async (source, destination, amount.number (), [response_a] (std::unique_ptr <rai::block> block_a)
							{
								rai::uint256_union hash (0);
								if (block_a != nullptr)
								{
									hash = block_a->hash ();
								}
								boost::property_tree::ptree response_l;
								response_l.put ("block", hash.to_string ());
								response_a (response_l);
							});
						}
						else
						{
							error_response (response, "Bad amount format");
						}
					}
					else
					{
						error_response (response, "Bad destination account");
					}
				}
				else
				{
					error_response (response, "Bad source account");
				}
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad wallet number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::stop ()
{
	if (rpc.config.enable_control)
	{
		rpc.stop ();
		node.stop ();
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::version ()
{
	boost::property_tree::ptree response_l;
	response_l.put ("rpc_version", "1");
	response_l.put ("store_version", std::to_string (node.store_version ()));
	response_l.put ("node_vendor", boost::str (boost::format ("RaiBlocks %1%.%2%.%3%") % RAIBLOCKS_VERSION_MAJOR % RAIBLOCKS_VERSION_MINOR % RAIBLOCKS_VERSION_PATCH));
	response (response_l);
}

void rai::rpc_handler::validate_account_number ()
{
	std::string account_text (request.get <std::string> ("account"));
	rai::uint256_union account;
	auto error (account.decode_account (account_text));
	boost::property_tree::ptree response_l;
	response_l.put ("valid", error ? "0" : "1");
	response (response_l);
}

void rai::rpc_handler::wallet_add ()
{
	if (rpc.config.enable_control)
	{
		std::string key_text (request.get <std::string> ("key"));
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::raw_key key;
		auto error (key.data.decode_hex (key_text));
		if (!error)
		{
			rai::uint256_union wallet;
			auto error (wallet.decode_hex (wallet_text));
			if (!error)
			{
				auto existing (node.wallets.items.find (wallet));
				if (existing != node.wallets.items.end ())
				{
					auto pub (existing->second->insert_adhoc (key));
					if (!pub.is_zero ())
					{
						boost::property_tree::ptree response_l;
						response_l.put ("account", pub.to_account ());
						response (response_l);
					}
					else
					{
						error_response (response, "Wallet locked");
					}
				}
				else
				{
					error_response (response, "Wallet not found");
				}
			}
			else
			{
				error_response (response, "Bad wallet number");
			}
		}
		else
		{
			error_response (response, "Bad private key");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::wallet_balance_total ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			rai::uint128_t balance (0);
			rai::uint128_t pending (0);
			rai::transaction transaction (node.store.environment, nullptr, false);
			for (auto i (existing->second->store.begin (transaction)), n (existing->second->store.end ()); i != n; ++i)
			{
				rai::account account(i->first);
				balance = balance + node.ledger.account_balance (transaction, account);
				pending = pending + node.ledger.account_pending (transaction, account);
			}
			boost::property_tree::ptree response_l;
			response_l.put ("balance", balance.convert_to <std::string> ());
			response_l.put ("pending", pending.convert_to <std::string> ());
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad wallet number");
	}
}

void rai::rpc_handler::wallet_balances ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree balances;
			rai::transaction transaction (node.store.environment, nullptr, false);
			for (auto i (existing->second->store.begin (transaction)), n (existing->second->store.end ()); i != n; ++i)
			{
				rai::account account(i->first);
				boost::property_tree::ptree entry;
				rai::uint128_t balance = node.ledger.account_balance (transaction, account);
				rai::uint128_t pending = node.ledger.account_pending (transaction, account);
				entry.put ("balance", balance.convert_to <std::string> ());
				entry.put ("pending", pending.convert_to <std::string> ());
				balances.push_back (std::make_pair (account.to_account (), entry));
			}
			response_l.add_child ("balances", balances);
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad wallet number");
	}
}

void rai::rpc_handler::wallet_change_seed ()
{
	if (rpc.config.enable_control)
	{
		std::string seed_text (request.get <std::string> ("seed"));
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::raw_key seed;
		auto error (seed.data.decode_hex (seed_text));
		if (!error)
		{
			rai::uint256_union wallet;
			auto error (wallet.decode_hex (wallet_text));
			if (!error)
			{
				auto existing (node.wallets.items.find (wallet));
				if (existing != node.wallets.items.end ())
				{
					rai::transaction transaction (node.store.environment, nullptr, true);
					if (existing->second->store.valid_password (transaction))
					{
						existing->second->store.seed_set (transaction, seed);
						boost::property_tree::ptree response_l;
						response_l.put ("success", "");
						response (response_l);
					}
					else
					{
						error_response (response, "Wallet locked");
					}
				}
				else
				{
					error_response (response, "Wallet not found");
				}
			}
			else
			{
				error_response (response, "Bad wallet number");
			}
		}
		else
		{
			error_response (response, "Bad seed");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::wallet_contains ()
{
	std::string account_text (request.get <std::string> ("account"));
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union account;
	auto error (account.decode_account (account_text));
	if (!error)
	{
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				rai::transaction transaction (node.store.environment, nullptr, false);
				auto exists (existing->second->store.find (transaction, account) != existing->second->store.end ());
				boost::property_tree::ptree response_l;
				response_l.put ("exists", exists ? "1" : "0");
				response (response_l);
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad wallet number");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::wallet_create ()
{
	if (rpc.config.enable_control)
	{
		rai::keypair wallet_id;
		auto wallet (node.wallets.create (wallet_id.pub));
		boost::property_tree::ptree response_l;
		response_l.put ("wallet", wallet_id.pub.to_string ());
		response (response_l);
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::wallet_destroy ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				node.wallets.destroy (wallet);
				boost::property_tree::ptree response_l;
				response (response_l);
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad wallet number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::wallet_export ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			rai::transaction transaction (node.store.environment, nullptr, false);
			std::string json;
			existing->second->store.serialize_json (transaction, json);
			boost::property_tree::ptree response_l;
			response_l.put ("json", json);
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::wallet_frontiers ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			boost::property_tree::ptree response_l;
			boost::property_tree::ptree frontiers;
			rai::transaction transaction (node.store.environment, nullptr, false);
			for (auto i (existing->second->store.begin (transaction)), n (existing->second->store.end ()); i != n; ++i)
			{
				rai::account account(i->first);
				auto latest (node.ledger.latest (transaction, account));
				if (!latest.is_zero ())
				{
					frontiers.put (account.to_account (), latest.to_string ());
				}
			}
			response_l.add_child ("frontiers", frontiers);
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad wallet number");
	}
}

void rai::rpc_handler::wallet_key_valid ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			rai::transaction transaction (node.store.environment, nullptr, false);
			auto valid (existing->second->store.valid_password (transaction));
			boost::property_tree::ptree response_l;
			response_l.put ("valid", valid ? "1" : "0");
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad wallet number");
	}
}

void rai::rpc_handler::wallet_representative ()
{
	std::string wallet_text (request.get <std::string> ("wallet"));
	rai::uint256_union wallet;
	auto error (wallet.decode_hex (wallet_text));
	if (!error)
	{
		auto existing (node.wallets.items.find (wallet));
		if (existing != node.wallets.items.end ())
		{
			rai::transaction transaction (node.store.environment, nullptr, false);
			boost::property_tree::ptree response_l;
			response_l.put ("representative", existing->second->store.representative (transaction).to_account ());
			response (response_l);
		}
		else
		{
			error_response (response, "Wallet not found");
		}
	}
	else
	{
		error_response (response, "Bad account number");
	}
}

void rai::rpc_handler::wallet_representative_set ()
{
	if (rpc.config.enable_control)
	{
		std::string wallet_text (request.get <std::string> ("wallet"));
		rai::uint256_union wallet;
		auto error (wallet.decode_hex (wallet_text));
		if (!error)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				std::string representative_text (request.get <std::string> ("representative"));
				rai::account representative;
				auto error (representative.decode_account (representative_text));
				if (!error)
				{
					rai::transaction transaction (node.store.environment, nullptr, true);
					existing->second->store.representative_set (transaction, representative);
					boost::property_tree::ptree response_l;
					response_l.put ("set", "1");
					response (response_l);
				}
				else
				{
					error_response (response, "Invalid account number");
				}
			}
			else
			{
				error_response (response, "Wallet not found");
			}
		}
		else
		{
			error_response (response, "Bad account number");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::work_generate ()
{
	if (rpc.config.enable_control)
	{
		std::string hash_text (request.get <std::string> ("hash"));
		rai::block_hash hash;
		auto error (hash.decode_hex (hash_text));
		if (!error)
		{
			auto rpc_l (shared_from_this ());
			node.work.generate (hash, [rpc_l] (boost::optional <uint64_t> const & work_a)
			{
				if (work_a)
				{
					boost::property_tree::ptree response_l;
					response_l.put ("work", rai::to_string_hex (work_a.value ()));
					rpc_l->response (response_l);
				}
				else
				{
					error_response (rpc_l->response, "Cancelled");
				}
			});
		}
		else
		{
			error_response (response, "Bad block hash");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::work_cancel ()
{
	if (rpc.config.enable_control)
	{
		std::string hash_text (request.get <std::string> ("hash"));
		rai::block_hash hash;
		auto error (hash.decode_hex (hash_text));
		if (!error)
		{
			node.work.cancel (hash);
			boost::property_tree::ptree response_l;
			response (response_l);
		}
		else
		{
			error_response (response, "Bad block hash");
		}
	}
	else
	{
		error_response (response, "RPC control is disabled");
	}
}

void rai::rpc_handler::work_validate ()
{
	std::string hash_text (request.get <std::string> ("hash"));
	rai::block_hash hash;
	auto error (hash.decode_hex (hash_text));
	if (!error)
	{
		std::string work_text (request.get <std::string> ("work"));
		uint64_t work;
		auto work_error (rai::from_string_hex (work_text, work));
		if (!work_error)
		{
			auto validate (node.work.work_validate (hash, work));
			boost::property_tree::ptree response_l;
			response_l.put ("valid", validate ? "0" : "1");
			response (response_l);
		}
		else
		{
			error_response (response, "Bad work");
		}
	}
	else
	{
		error_response (response, "Bad block hash");
	}
}

rai::rpc_connection::rpc_connection (rai::node & node_a, rai::rpc & rpc_a) :
node (node_a.shared ()),
rpc (rpc_a),
socket (node_a.network.service)
{
}

void rai::rpc_connection::parse_connection ()
{
	auto this_l (shared_from_this ());
	beast::http::async_read (socket, buffer, request, [this_l] (boost::system::error_code const & ec)
	{
		if (!ec)
		{
			this_l->node->background ([this_l] ()
			{
				auto start (std::chrono::system_clock::now ());
				auto version (this_l->request.version);
				auto response_handler ([this_l, version, start] (boost::property_tree::ptree const & tree_a)
				{
					std::stringstream ostream;
					boost::property_tree::write_json (ostream, tree_a);
					ostream.flush ();
					auto body (ostream.str ());
					this_l->res.fields.insert ("content-type", "application/json");
					this_l->res.fields.insert ("Access-Control-Allow-Origin",  "*");
					this_l->res.status = 200;
					this_l->res.body = body;
					this_l->res.version = version;
					beast::http::prepare (this_l->res);
					beast::http::async_write (this_l->socket, this_l->res, [this_l] (boost::system::error_code const & ec)
					{
					});
					if (this_l->node->config.logging.log_rpc ())
					{
						BOOST_LOG (this_l->node->log) << boost::str (boost::format ("RPC request %2% completed in: %1% microseconds") % std::chrono::duration_cast <std::chrono::microseconds> (std::chrono::system_clock::now () - start).count () % boost::io::group (std::hex, std::showbase, reinterpret_cast <uintptr_t> (this_l.get ())));
					}
				});
				if (this_l->request.method () == "POST")
				{
					auto handler (std::make_shared <rai::rpc_handler> (*this_l->node, this_l->rpc, this_l->request.body, response_handler));
					handler->process_request ();
				}
				else
				{
					error_response (response_handler, "Can only POST requests");
				}
			});
		}
	});
}

namespace
{
void reprocess_body (std::string & body, boost::property_tree::ptree & tree_a)
{
	std::stringstream stream;
	boost::property_tree::write_json (stream, tree_a);
	body = stream.str ();
}
}

void rai::rpc_handler::process_request ()
{
	try
	{
		std::stringstream istream (body);
		boost::property_tree::read_json (istream, request);
		std::string action (request.get <std::string> ("action"));
		if (action == "password_enter")
		{
			password_enter ();
			request.erase ("password");
			reprocess_body (body, request);
		}
		else if (action == "password_change")
		{
			password_change ();
			request.erase ("password");
			reprocess_body (body, request);
		}
		if (node.config.logging.log_rpc ())
		{
			BOOST_LOG (node.log) << body;
		}
		if (action == "account_balance")
		{
			account_balance ();
		}
		else if (action == "account_block_count")
		{
			account_block_count ();
		}
		else if (action == "account_create")
		{
			account_create ();
		}
		else if (action == "account_get")
		{
			account_get ();
		}
		else if (action == "account_history")
		{
			account_history ();
		}
		else if (action == "account_key")
		{
			account_key ();
		}
		else if (action == "account_list")
		{
			account_list ();
		}
		else if (action == "account_move")
		{
			account_move ();
		}
		else if (action == "account_remove")
		{
			account_remove ();
		}
		else if (action == "account_representative")
		{
			account_representative ();
		}
		else if (action == "account_representative_set")
		{
			account_representative_set ();
		}
		else if (action == "account_weight")
		{
			account_weight ();
		}
		else if (action == "accounts_balances")
		{
			accounts_balances ();
		}
		else if (action == "accounts_frontiers")
		{
			accounts_frontiers ();
		}
		else if (action == "accounts_pending")
		{
			accounts_pending ();
		}
		else if (action == "available_supply")
		{
			available_supply ();
		}
		else if (action == "block")
		{
			block ();
		}
		else if (action == "blocks")
		{
			blocks ();
		}
		else if (action == "block_account")
		{
			block_account ();
		}
		else if (action == "block_count")
		{
			block_count ();
		}
		else if (action == "successors")
		{
			successors ();
		}
		else if (action == "bootstrap")
		{
			bootstrap ();
		}
		else if (action == "bootstrap_any")
		{
			bootstrap_any ();
		}
		else if (action == "chain")
		{
			chain ();
		}
		else if (action == "deterministic_key")
		{
			deterministic_key ();
		}
		else if (action == "frontiers")
		{
			frontiers ();
		}
		else if (action == "frontier_count")
		{
			frontier_count ();
		}
		else if (action == "history")
		{
			history ();
		}
		else if (action == "keepalive")
		{
			keepalive ();
		}
		else if (action == "key_create")
		{
			key_create ();
		}
		else if (action == "key_expand")
		{
			key_expand ();
		}
		else if (action == "krai_from_raw")
		{
			krai_from_raw ();
		}
		else if (action == "krai_to_raw")
		{
			krai_to_raw ();
		}
		else if (action == "mrai_from_raw")
		{
			mrai_from_raw ();
		}
		else if (action == "mrai_to_raw")
		{
			mrai_to_raw ();
		}
		else if (action == "password_change")
		{
			// Processed before logging
		}
		else if (action == "password_enter")
		{
			// Processed before logging
		}
		else if (action == "password_valid")
		{
			password_valid ();
		}
		else if (action == "payment_begin")
		{
			payment_begin ();
		}
		else if (action == "payment_init")
		{
			payment_init ();
		}
		else if (action == "payment_end")
		{
			payment_end ();
		}
		else if (action == "payment_wait")
		{
			payment_wait ();
		}
		else if (action == "peers")
		{
			peers ();
		}
		else if (action == "pending")
		{
			pending ();
		}
		else if (action == "process")
		{
			process ();
		}
		else if (action == "rai_from_raw")
		{
			rai_from_raw ();
		}
		else if (action == "rai_to_raw")
		{
			rai_to_raw ();
		}
		else if (action == "receive")
		{
			receive ();
		}
		else if (action == "representatives")
		{
			representatives ();
		}
		else if (action == "republish")
		{
			republish ();
		}
		else if (action == "search_pending")
		{
			search_pending ();
		}
		else if (action == "send")
		{
			send ();
		}
		else if (action == "stop")
		{
			stop ();
		}
		else if (action == "validate_account_number")
		{
			validate_account_number ();
		}
		else if (action == "version")
		{
			version ();
		}
		else if (action == "wallet_add")
		{
			wallet_add ();
		}
		else if (action == "wallet_balance_total")
		{
			wallet_balance_total ();
		}
		else if (action == "wallet_balances")
		{
			wallet_balances ();
		}
		else if (action == "wallet_change_seed")
		{
			wallet_change_seed ();
		}
		else if (action == "wallet_contains")
		{
			wallet_contains ();
		}
		else if (action == "wallet_create")
		{
			wallet_create ();
		}
		else if (action == "wallet_destroy")
		{
			wallet_destroy ();
		}
		else if (action == "wallet_export")
		{
			wallet_export ();
		}
		else if (action == "wallet_frontiers")
		{
			wallet_frontiers ();
		}
		else if (action == "wallet_key_valid")
		{
			wallet_key_valid ();
		}
		else if (action == "wallet_representative")
		{
			wallet_representative ();
		}
		else if (action == "wallet_representative_set")
		{
			wallet_representative_set ();
		}
		else if (action == "work_generate")
		{
			work_generate ();
		}
		else if (action == "work_cancel")
		{
			work_cancel ();
		}
		else if (action == "work_validate")
		{
			work_validate ();
		}
		else
		{
			error_response (response, "Unknown command");
		}
	}
	catch (std::runtime_error const & err)
	{
		error_response (response, "Unable to parse JSON");
	}
	catch (...)
	{
		error_response (response, "Internal server error in RPC");
	}
}

rai::payment_observer::payment_observer (std::function <void (boost::property_tree::ptree const &)> const & response_a, rai::rpc & rpc_a, rai::account const & account_a, rai::amount const & amount_a) :
rpc (rpc_a),
account (account_a),
amount (amount_a),
response (response_a)
{
	completed.clear ();
}

void rai::payment_observer::start (uint64_t timeout)
{
	auto this_l (shared_from_this ());
	rpc.node.alarm.add (std::chrono::system_clock::now () + std::chrono::milliseconds (timeout), [this_l] ()
	{
		this_l->complete (rai::payment_status::nothing);
	});
}

rai::payment_observer::~payment_observer ()
{
}

void rai::payment_observer::observe ()
{
	if (rpc.node.balance (account) >= amount.number ())
	{
		complete (rai::payment_status::success);
	}
}

void rai::payment_observer::complete (rai::payment_status status)
{
	auto already (completed.test_and_set ());
	if (!already)
	{
		if (rpc.node.config.logging.log_rpc ())
		{
			BOOST_LOG (rpc.node.log) << boost::str (boost::format ("Exiting payment_observer for account %1% status %2%") % account.to_account () % static_cast <unsigned> (status));
		}
		switch (status)
		{
			case rai::payment_status::nothing:
			{
				boost::property_tree::ptree response_l;
				response_l.put ("status", "nothing");
				response (response_l);
				break;
			}
			case rai::payment_status::success:
			{
				boost::property_tree::ptree response_l;
				response_l.put ("status", "success");
				response (response_l);
				break;
			}
			default:
			{
				error_response (response, "Internal payment error");
				break;
			}
		}
		std::lock_guard <std::mutex> lock (rpc.mutex);
		assert (rpc.payment_observers.find (account) != rpc.payment_observers.end ());
		rpc.payment_observers.erase (account);
	}
}
