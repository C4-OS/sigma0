#include <sigma0/sigma0.h>
#include <sigma0/tar.h>
#include <sigma0/elf.h>
#include <c4/thread.h>
#include <c4/bootinfo.h>
#include <c4rt/c4rt.h>
#include <c4rt/stublibc.h>

#define DBG_PRINTF( FORMAT, ... ) \
	c4_debug_printf( "--- sigma0: " FORMAT, __VA_ARGS__ )

typedef struct c4_process {
	int addrspace;
	int capspace;
	int endpoint;
	int thread;
	bool running;
} c4_process_t;

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

c4_process_t elf_load( Elf32_Ehdr *elf, int nameserver );
c4_process_t elf_load_file( const char *name, int nameserver );
c4_process_t elf_load_tar_header( tar_header_t *header, int nameserver );

static void  bss_init( void );
//static void *allot_pages( unsigned pages );
static int allot_pages( unsigned pages );
static void  server( void *data );
static void  test_thread( void );

void main( void ){
	struct foo thing;

	// set the memory in bss to zero, which is needed since this loaded
	// as a flat binary
	bss_init( );
	int ret = 0;
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

static void test_thread( void ){
	message_t msg = {
		.type = 123456,
		.data = {1, 2, 3, 4, 5},
	};

	*(volatile unsigned *)0 = 1234;

	c4_msg_send( &msg, 1 );
	//DBG_PRINTF( "got here man, %u\n", 123 );
	c4_debug_printf( "--- sigma0: testing this\n" );
	//for (;;);
	c4_exit();
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

	while ( true ){
		c4_msg_recieve( &msg, 1 );

		if ( is_bootinfo_fault( &msg )){
			DBG_PRINTF( "bootinfo memory request from %u\n", msg.sender );
			c4_mem_map_to( msg.sender, BOOTINFO_ADDR, BOOTINFO_ADDR,
			               1, PAGE_READ );
			c4_continue_thread( msg.sender );

		} else if ( is_page_fault( &msg )){
			c4_debug_printf(
				"--- sigma0: unhandled page fault: thread %u, %s %p, ip=%p\n",
				msg.sender,
				(msg.data[2] == PAGE_WRITE)? "write to" : "read from",
				msg.data[0], msg.data[1]
			);

			//c4_dump_maps( msg.sender );

		} else if ( is_object_grant( &msg )){
			int32_t obj = msg.data[5];
			message_t objmsg;

			c4_msg_recieve( &objmsg, obj );

			if ( is_page_request(&objmsg) ){
				handle_page_request( obj, &objmsg );
			}

			/*
		} else if ( is_page_request( &msg )){
			DBG_PRINTF( "request for %u pages with permissions %b\n",
				msg.data[1], msg.data[0] );

			// TODO: two things, keep track of available memory, and check that
			//       there is actually enough memory to satisfy the request
			unsigned pages = msg.data[1];
			//void *page = allot_pages( pages );
			//void *page = NULL;
			//void *addr = (void *)msg.data[0];
			unsigned permissions = msg.data[1];
			unsigned long sender = msg.sender;

			//c4_mem_grant_to( sender, page, addr, pages, permissions );
			*/

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

static int allot_pages( unsigned pages ){
	int ret = phys_page_buffer;
	phys_page_buffer = c4_phys_frame_split( phys_page_buffer, pages );
	return ret;
}

static inline void elf_load_set_arg( uint8_t *stack,
                                     unsigned offset,
                                     unsigned arg,
                                     unsigned value )
{
	*((unsigned *)(stack + offset) + arg + 1) = value;
}

#define BUGOUT(X) \
	if ( (X) < 0 ){\
		c4_debug_printf("--- sigma0: ERROR: %u: %u\n", __LINE__, -(X));\
	}

c4_process_t elf_load( Elf32_Ehdr *elf, int nameserver ){
	c4_process_t ret;

	unsigned stack_offset = 0xfe0;
	int frame = allot_pages(1);
	int cspace = c4_cspace_create();
	int aspace = c4_addrspace_create();

	void *entry          = (void *)elf->e_entry;
	uintptr_t to_stack   = 0xa0000000;
	uintptr_t from_stack = 0xda000000;
	uintptr_t stack      = to_stack + stack_offset;

	int c4ret = 0;

	int thread_id = c4_create_thread( entry, (void *)stack, 0 );
	c4_debug_printf( "--- sigma0: made thread %u\n", thread_id );
	BUGOUT(thread_id);

	c4_set_addrspace( thread_id, aspace );
	c4_set_capspace( thread_id, cspace );
	c4_set_pager( thread_id, 1 );
	int msgq = c4_msg_create_sync();

	c4_cspace_copy( C4_CURRENT_CSPACE, msgq,         cspace, C4_SERV_PORT );
	c4_cspace_copy( C4_CURRENT_CSPACE, nameserver,   cspace, C4_NAMESERVER );
	c4_cspace_copy( C4_CURRENT_CSPACE, aspace,       cspace, C4_CURRENT_ADDRSPACE );
	c4_cspace_copy( C4_CURRENT_CSPACE, C4_SERV_PORT, cspace, C4_PAGER );

	c4_cspace_restrict( cspace, nameserver,
	                    CAP_MODIFY | CAP_SHARE | CAP_MULTI_USE );

	ret.addrspace = aspace;
	ret.capspace  = cspace;
	ret.thread    = thread_id;
	ret.endpoint  = msgq;

	c4ret = c4_addrspace_map( 2, frame, from_stack, PAGE_READ | PAGE_WRITE );
	BUGOUT(c4ret);
	c4ret = c4_addrspace_map( aspace, frame, to_stack, PAGE_READ | PAGE_WRITE );
	BUGOUT(c4ret);
	elf_load_set_arg( (void *)from_stack, stack_offset, 0, C4_NAMESERVER );
	c4ret = c4_addrspace_unmap( 2, from_stack );
	BUGOUT(c4ret);

	// load program headers
	for ( unsigned i = 0; i < elf->e_phnum; i++ ){
		Elf32_Phdr *header = elf_get_phdr( elf, i );
		uint8_t *progdata  = (uint8_t *)elf + header->p_offset;
		uintptr_t addr     = header->p_vaddr;
		unsigned pages     = (header->p_memsz / PAGE_SIZE)
		                   + (header->p_memsz % PAGE_SIZE > 0);
		int frame          = allot_pages( pages );
		uintptr_t databuf  = 0xda000000;
		uint8_t *dataptr   = (void *)databuf;
		unsigned offset    = header->p_vaddr % PAGE_SIZE;
		uintptr_t adjaddr  = addr - offset;

		// TODO: translate elf permissions into message permissions
		c4_addrspace_map( 2,      frame, databuf, PAGE_READ | PAGE_WRITE );
		c4_addrspace_map( aspace, frame, adjaddr, PAGE_READ | PAGE_WRITE );

		for ( unsigned k = 0; k < header->p_filesz; k++ ){
			dataptr[k + offset] = progdata[k];
		}

		c4_addrspace_unmap( 2, databuf );
		c4_cspace_move( 0, frame, cspace, i + C4_DEFAULT_OBJECT_END );

		/*
		c4_mem_grant_to( thread_id, databuf, adjaddr, pages,
		                 PAGE_READ | PAGE_WRITE );
		 */
	}

	c4_continue_thread( thread_id );
	return ret;
}

c4_process_t elf_load_file( const char *name, int nameserver ){
	//c4_process_t ret = -1;
	c4_process_t ret;
	tar_header_t *lookup = tar_lookup( tar_initfs, name );

	if ( lookup ){
		void *data = tar_data( lookup );

		ret = elf_load( data, nameserver );
	}

	return ret;
}

c4_process_t elf_load_tar_header( tar_header_t *header, int nameserver ){
	c4_process_t ret;

	if ( header ){
		void *data = tar_data( header );

		ret = elf_load( data, nameserver );
	}

	return ret;
}
