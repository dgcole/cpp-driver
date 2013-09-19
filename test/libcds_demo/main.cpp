#include <cstdlib>
#include <iostream>

#include <functional>

#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/asio/ip/address.hpp>

#include <cds/init.h>
#include <cds/gc/hp.h>

#include <cds/opt/hash.h>
#include <cds/container/michael_list_hp.h>
#include <cds/container/split_list_map.h>
namespace cc = cds::container;



#include "lockfree_hash_map.hpp"
using namespace cql;

namespace std {

	template<>
	struct hash<::boost::asio::ip::address> {
	public:
		typedef 
			::boost::asio::ip::address 
			argument_type;

		typedef 
			::std::size_t
			result_type;

	private:
		template<typename TBytesType>
		inline result_type
		get_hash(const TBytesType& ip_bytes) const {
			// implemented using djb2 algorithm
			// see: http://www.cse.yorku.ca/~oz/hash.html

			unsigned long hash = 5381;
			
			for(TBytesType::const_iterator it = ip_bytes.cbegin(); 
				it != ip_bytes.cend(); ++it) 
			{
				hash = ((hash << 5) + hash) + *it;
			}

			return static_cast<result_type>(hash);
		}

	public:
		inline result_type 
		operator ()(const argument_type& address) const {
			if(address.is_v4()) {
				return get_hash(address.to_v4().to_bytes());
			}
			else {
				return get_hash(address.to_v6().to_bytes());
			}
		}
	};

	template<>
	struct less<::boost::asio::ip::address> {
	public:
		typedef 
			::boost::asio::ip::address
			ip_addr;

		typedef
			ip_addr
			first_argument_type;

		typedef
			ip_addr
			second_argument_type;

		typedef
			bool
			result_type;

	private:
		typedef
			::boost::asio::ip::address_v6
			ip_v6;

		inline ip_v6
		to_ip_v6(const ip_addr& address) const {
			return address.is_v6() 
				? address.to_v6() 
				: ip_v6::v4_mapped(address.to_v4());
		}

	public:
		inline result_type
		operator ()(const ip_addr& first, const ip_addr& second) const {
			ip_v6 first_v6 = to_ip_v6(first);
			ip_v6 second_v6 = to_ip_v6(second);

			ip_v6::bytes_type first_bytes = first_v6.to_bytes();
			ip_v6::bytes_type second_bytes = second_v6.to_bytes();

			ip_v6::bytes_type::const_iterator first_it = first_bytes.cbegin();
			ip_v6::bytes_type::const_iterator second_it = second_bytes.cbegin();

			for(; first_it != first_bytes.cend(); ++first_it, ++second_it) {
				int delta = (int)*first_it - (int)*second_it;
				if(delta != 0)
					return delta < 0;
			}

			// both addresses are equal
			return false;
		}
	};
}

#define IP4(b1, b2, b3, b4) ip4(b4 + 256U * (b3 + 256U * (b2 + 256U * b1)))


void
do_job() {
    typedef 
		::boost::asio::ip::address 
		ip_addr;

	typedef 
		::boost::asio::ip::address_v4
		ip4;
	
	using boost::shared_ptr;
	
	lockfree_hash_map_t<ip_addr, shared_ptr<std::string> > h;

	ip_addr a1(IP4(10, 0, 8, 1));
	ip_addr a2(IP4(10, 0, 8, 2));
	ip_addr a3(IP4(10, 8, 2, 1));
	ip_addr a4(IP4(192, 168, 0, 1));

	h.try_add(a1, shared_ptr<std::string>(new std::string("a1")));
	h.try_add(a2, shared_ptr<std::string>(new std::string("a2")));
	h.try_add(a3, shared_ptr<std::string>(new std::string("a3")));
	h.try_add(a4, shared_ptr<std::string>(new std::string("a4")));

	std::cout << "h.size() = " << h.size() << std::endl;

	std::cout << "keys: " << std::endl;
	std::vector<ip_addr> ips;
	h.unsafe_get_keys(back_inserter(ips));
	for(auto it = ips.begin(); it != ips.end(); ++it) 
		std::cout << it->to_string() <<  std::endl;

	std::vector<shared_ptr<std::string>> names;
	h.unsafe_get_values(back_inserter(names));
	for(auto it = names.begin(); it != names.end(); ++it) {
		std::cout << **it << std::endl;
	}

	shared_ptr<std::string> result;
	h.try_get(a1, &result);
	std::cout << "h[a1]: " << *result << std::endl;

	std::cout << "h[non-ex] " << h.try_get(ip_addr(IP4(100,100,0,0)), &result) << std::endl;
}

int 
main(int argc, char* argv[]) {
    cds::Initialize();
    {
        cds::gc::HP hp_infrastructure;
        cds::gc::HP::thread_gc hp_thread_gc;
    
        do_job();
    }
    cds::Terminate();
}
