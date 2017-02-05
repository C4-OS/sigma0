#include <sigma0/sigma0.h>
#include <sigma0/tar.h>
#include <sigma0/elf.h>
#include <c4/thread.h>
#include <c4/bootinfo.h>
#include <c4rt/c4rt.h>

struct foo {
	int target;
	int display;
	int nameserver;
};

static bootinfo_t *c4_bootinfo = (void *)0xfcfff000;

extern char initfs_start[];
extern char initfs_end[];

static tar_header_t *tar_initfs = (void *)initfs_start;

int  elf_load( Elf32_Ehdr *elf, int nameserver );
int  elf_load_file( const char *name, int nameserver );

static void  bss_init( void );
static void  framebuffer_init( void );
static void *allot_pages( unsigned pages );

void main( void ){
	struct foo thing;

	// set the memory in bss to zero, which is needed since this loaded
	// as a flat binary
	bss_init( );
	framebuffer_init();

	thing.nameserver = elf_load_file( "./bin/nameserver", 4 );
	thing.display    = elf_load_file( "./bin/display", thing.nameserver );

	elf_load_file( "./bin/pci", thing.display );
	elf_load_file( "./bin/keyboard", thing.nameserver );
	elf_load_file( "./bin/forth", thing.nameserver );

	server( &thing );

	// TODO: panic or dump debug info or something, server()
	//       should never return
	for ( ;; );
}

static void bss_init( void ){
	extern uint8_t bss_start;
	extern uint8_t bss_end;

	for ( uint8_t *ptr = &bss_start; ptr < &bss_end; ptr++ ){
		*ptr = 0;
	}
}

// TODO: move this to the display program
static void framebuffer_init( void ){
	if ( c4_bootinfo->framebuffer.exists ){
		unsigned size =
			c4_bootinfo->framebuffer.width *
			c4_bootinfo->framebuffer.height *
			4;

		c4_request_physical( 0xfb000000,
		                     c4_bootinfo->framebuffer.addr,
		                     size / PAGE_SIZE + 1,
		                     PAGE_READ | PAGE_WRITE );

		uint32_t *fb = (void *)0xfb000000;

		for ( unsigned y = 0; y < c4_bootinfo->framebuffer.height; y++ ){
			for ( unsigned x = 0; x < c4_bootinfo->framebuffer.width; x++ ){
				unsigned index = y * c4_bootinfo->framebuffer.width + x;

				fb[index] = ((y & 0xff) << 16) | ((x & 0xff) << 8) | (x ^ y);
			}
		}
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

extern const char *foo;

static inline bool is_page_fault( const message_t *msg ){
	return msg->type == MESSAGE_TYPE_PAGE_FAULT;
}

static inline bool is_page_request( const message_t *msg ){
	return msg->type == 0xbeef10af;
}

static inline bool is_keystroke( const message_t *msg ){
	return msg->type == 0xbeef;
}

void server( void *data ){
	message_t msg;

	while ( true ){
		c4_msg_recieve( &msg, 0 );

		if ( is_page_fault( &msg )){
			c4_debug_printf(
				"--- sigma0: unhandled page fault: thread %u, %s %p, ip=%p\n",
				msg.sender,
				(msg.data[2] == PAGE_WRITE)? "write to" : "read from",
				msg.data[0], msg.data[1]
			);

			c4_dump_maps( msg.sender );

		} else if ( is_page_request( &msg )){
			c4_debug_printf( "--- sigma0: got a page request for %p\n",
			                 msg.data[0] );

			void *page = allot_pages( 1 );

			c4_mem_grant_to( msg.sender, page,
			                 (void *)msg.data[0], 1, msg.data[1] );

		} else {
			c4_debug_printf( "--- sigma0: unknown message %x\n", msg.type );
		}
	}

	for ( ;; );
}
