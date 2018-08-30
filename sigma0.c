#include <sigma0/sigma0.h>
#include <sigma0/tar.h>
#include <c4/thread.h>
#include <c4/bootinfo.h>
#include <c4rt/c4rt.h>
#include <c4rt/stublibc.h>
#include <c4rt/elf.h>

#define DBG_PRINTF( FORMAT, ... ) \
	c4_debug_printf( "--- sigma0: " FORMAT, __VA_ARGS__ )

struct foo {
	int target;
	int display;
	c4_process_t nameserver;
};

extern char initfs_start[];
extern char initfs_end[];

static tar_header_t *tar_initfs = (void *)initfs_start;
// TODO: make a tree of available pages
int phys_page_buffer = -1;

//c4_process_t elf_load( Elf32_Ehdr *elf, int nameserver );
c4_process_t elf_load_file( const char *name, int nameserver );
c4_process_t elf_load_tar_header( tar_header_t *header, int nameserver );

static void  bss_init( void );
//static void *allot_pages( unsigned pages );
static int32_t allot_pages( unsigned pages );
static void  server( void *data );

// XXX: avoid using the libc _start()
void main(void);
void _start(void){
	main();
}

void main(void){
	struct foo thing;

	// set the memory in bss to zero, which is needed since this loaded
	// as a flat binary
	bss_init( );
	message_t msg;

	do {
		c4_msg_recieve( &msg, 1 );

		switch ( msg.type ){
			case MESSAGE_TYPE_GRANT_OBJECT:
				{
					uint32_t frame = msg.data[5];
					c4_debug_printf( "--- sigma0: recieved a capability "
					                 "at %u, type: %u\n",
					                 msg.data[5], msg.data[0] );

					if ( phys_page_buffer < 0 ){
						phys_page_buffer = frame;
					}
				}
				break;

			default:
				c4_debug_printf( "--- got some other crap: %u\n", msg.type );
				break;
		}

	} while ( msg.type != 1234 );

	thing.nameserver = elf_load_file( "./bin/nameserver", 0 );

	msg = (message_t){
		.type = 12345,
		.data = {0},
	};

	c4_debug_printf( "--- sigma0: exiting...\n" );

	for ( tar_header_t *iter = tar_initfs;
	      !tar_end(iter);
	      iter = tar_next( iter ))
	{
		// XXX: avoid loading the nameserver a second time
		if ( strcmp( iter->filename, "./bin/nameserver" ) == 0 ){
			continue;
		}

		elf_load_tar_header( iter, thing.nameserver.endpoint );
		c4_debug_printf( "--- sigma0: started \"%s\" at id %u\n",
				iter->filename, 0 );
	}

	server( &thing );

	// TODO: panic or dump debug info or something, server()
	//       should never return
	for ( ;; );
}

static inline bool is_page_fault( const message_t *msg ){
	return msg->type == MESSAGE_TYPE_PAGE_FAULT;
}

static inline bool is_page_request( const message_t *msg ){
	return msg->type == 0xbeef10af;
}

static inline bool is_keystroke( const message_t *msg ){
	return msg->type == 0xbeef;
}

static inline bool is_bootinfo_fault( const message_t *msg ){
	uintptr_t offset = msg->data[0] % PAGE_SIZE;
	uintptr_t addr   = msg->data[0] - offset;
	unsigned  perms  = msg->data[2];

	return is_page_fault( msg )
	    && addr  == (uintptr_t)BOOTINFO_ADDR
	    && perms == PAGE_READ;
}

static inline bool is_object_grant( const message_t *msg ){
	return msg->type == MESSAGE_TYPE_GRANT_OBJECT;
}

static void handle_page_request( int32_t responder, const message_t *msg ){
	DBG_PRINTF( "request for %u pages with permissions %b\n",
	            msg->data[1], msg->data[0] );

	// TODO: two things, keep track of available memory, and check that
	//       there is actually enough memory to satisfy the request
	unsigned pages = msg->data[1];
	int32_t page = allot_pages( pages );
	//void *page = allot_pages( pages );
	//void *page = NULL;
	//void *addr = (void *)msg.data[0];
	unsigned permissions = msg->data[0];
	//unsigned long sender = msg.sender;
	unsigned cap_permissions = CAP_MULTI_USE | CAP_SHARE;

	// translate page flags into permissions
	if ( permissions & PAGE_READ )  cap_permissions |= CAP_ACCESS;
	if ( permissions & PAGE_WRITE ) cap_permissions |= CAP_MODIFY;
	
	if ( page <= 0 ){
		DBG_PRINTF( "Got an error will trying to allocate %u pages!\n", pages );
		// TODO: properly handle this, this will currently leave the requester
		//       hanging while waiting for a response
		return;
	}
	
	c4_cspace_grant( page, responder, cap_permissions );
}

static void server( void *data ){
	message_t msg;

	while (true) {
		c4_msg_recieve(&msg, 1);

		if (is_page_fault(&msg)) {
			c4_debug_printf(
				"--- sigma0: unhandled page fault: thread %u, %s %p, ip=%p\n",
				msg.sender,
				(msg.data[2] == PAGE_WRITE)? "write to" : "read from",
				msg.data[0], msg.data[1]
			);

		} else if (is_object_grant(&msg)) {
			int32_t obj = msg.data[5];
			message_t objmsg;

			c4_msg_recieve(&objmsg, obj);

			if (is_page_request(&objmsg)) {
				handle_page_request(obj, &objmsg);
			}

		} else {
			DBG_PRINTF( "unknown message %x\n", msg.type );
		}
	}

	for ( ;; );
}

static void bss_init( void ){
	extern uint8_t bss_start;
	extern uint8_t bss_end;

	for ( uint8_t *ptr = &bss_start; ptr < &bss_end; ptr++ ){
		*ptr = 0;
	}
}

static int32_t allot_pages( unsigned pages ){
	int ret = phys_page_buffer;
	phys_page_buffer = c4_phys_frame_split( phys_page_buffer, pages );
	return ret;
}

static c4_process_t do_elf_load(void *data, int nameserver) {
	// XXX: add --no-forward argument for all processes started by sigma0,
	//      to ensure nameserver gets it as an argument
	//
	// TODO: an init script system, so all this can be configured
	const char *argv[] = {"[unknown]", "--no-forward", NULL};
	const char *envp[] = {NULL,};

	return elf_load_full(data, allot_pages, nameserver,
	                     C4_SERV_PORT, argv, envp);
}

c4_process_t elf_load_file( const char *name, int nameserver ){
	//c4_process_t ret = -1;
	c4_process_t ret;
	tar_header_t *lookup = tar_lookup( tar_initfs, name );

	if ( lookup ){
		void *data = tar_data( lookup );

		ret = do_elf_load(data, nameserver);
	}

	return ret;
}

c4_process_t elf_load_tar_header( tar_header_t *header, int nameserver ){
	c4_process_t ret;

	if ( header ){
		void *data = tar_data( header );

		ret = do_elf_load(data, nameserver);
	}

	return ret;
}
