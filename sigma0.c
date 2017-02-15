#include <sigma0/sigma0.h>
#include <sigma0/tar.h>
#include <sigma0/elf.h>
#include <c4/thread.h>
#include <c4/bootinfo.h>
#include <c4rt/c4rt.h>
#include <c4/bootinfo.h>

#define DBG_PRINTF( FORMAT, ... ) \
	c4_debug_printf( "--- sigma0: " FORMAT, __VA_ARGS__ )

struct foo {
	int target;
	int display;
	int nameserver;
};

extern char initfs_start[];
extern char initfs_end[];

static tar_header_t *tar_initfs = (void *)initfs_start;

int elf_load( Elf32_Ehdr *elf, int nameserver );
int elf_load_file( const char *name, int nameserver );

static void  bss_init( void );
static void *allot_pages( unsigned pages );
static void  server( void *data );

void main( void ){
	struct foo thing;

	// set the memory in bss to zero, which is needed since this loaded
	// as a flat binary
	bss_init( );

	thing.nameserver = elf_load_file( "./bin/nameserver", 4 );
	thing.display    = elf_load_file( "./bin/display", thing.nameserver );

	elf_load_file( "./bin/pci", thing.display );
	elf_load_file( "./bin/keyboard", thing.nameserver );
	elf_load_file( "./bin/forth", thing.nameserver );
	elf_load_file( "./bin/skeleton-prog", thing.nameserver );

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

static void server( void *data ){
	message_t msg;

	while ( true ){
		c4_msg_recieve( &msg, 0 );

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

			c4_dump_maps( msg.sender );

		} else if ( is_page_request( &msg )){
			DBG_PRINTF( "got a page request for %p\n", msg.data[0] );

			void *page = allot_pages( 1 );
			void *addr = (void *)msg.data[0];
			unsigned permissions = msg.data[1];
			unsigned long sender = msg.sender;

			c4_mem_grant_to( sender, page, addr, 1, permissions );

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

static void *allot_pages( unsigned pages ){
	static uint8_t *s = (void *)0xd0001000;
	void *ret = s;

	s += pages * PAGE_SIZE;

	return ret;
}

static inline void elf_load_set_arg( uint8_t *stack,
                                     unsigned offset,
                                     unsigned arg,
                                     unsigned value )
{
	*((unsigned *)(stack + offset) + arg + 1) = value;
}

int elf_load( Elf32_Ehdr *elf, int nameserver ){
	unsigned stack_offset = 0xff8;

	void *entry      = (void *)elf->e_entry;
	void *to_stack   = (uint8_t *)0xa0000000;
	void *from_stack = (uint8_t *)allot_pages(1);
	void *stack      = (uint8_t *)to_stack + stack_offset;

	// copy the output info to the new stack
	elf_load_set_arg( from_stack, stack_offset, 0, nameserver );

	int thread_id = c4_create_thread( entry, stack,
	                                  THREAD_CREATE_FLAG_NEWMAP);

	c4_mem_grant_to( thread_id, from_stack, to_stack, 1,
	                 PAGE_READ | PAGE_WRITE );

	// load program headers
	for ( unsigned i = 0; i < elf->e_phnum; i++ ){
		Elf32_Phdr *header = elf_get_phdr( elf, i );
		uint8_t *progdata  = (uint8_t *)elf + header->p_offset;
		void    *addr      = (void *)header->p_vaddr;
		unsigned pages     = (header->p_memsz / PAGE_SIZE)
		                   + (header->p_memsz % PAGE_SIZE > 0);
		uint8_t *databuf   = allot_pages( pages );
		unsigned offset    = header->p_vaddr % PAGE_SIZE;

		for ( unsigned k = 0; k < header->p_filesz; k++ ){
			databuf[k + offset] = progdata[k];
		}

		// TODO: translate elf permissions into message permissions
		c4_mem_grant_to( thread_id, databuf, addr, pages,
		                 PAGE_READ | PAGE_WRITE );
	}

	c4_set_pager( thread_id, 1 );
	c4_continue_thread( thread_id );

	return thread_id;
}

int elf_load_file( const char *name, int nameserver ){
	int ret = -1;
	tar_header_t *lookup = tar_lookup( tar_initfs, name );

	if ( lookup ){
		void *data = tar_data( lookup );

		ret = elf_load( data, nameserver );
	}

	return ret;
}
