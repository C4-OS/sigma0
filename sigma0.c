#include <sigma0/sigma0.h>
#include <sigma0/tar.h>
#include <sigma0/elf.h>
#include <c4/thread.h>
#include <c4/bootinfo.h>

struct foo {
	int target;
	int display;
	int nameserver;
};

static bootinfo_t *c4_bootinfo = (void *)0xfcfff000;

// tar
extern char _binary_initfs_tar_start[];
extern char _binary_initfs_tar_end[];

static tar_header_t *tar_initfs = (void *)_binary_initfs_tar_start;

int c4_set_pager( unsigned thread, unsigned pager );

void test_thread( void *unused );
void debug_putchar( char c );
void debug_print( struct foo *info, char *asdf );
int  elf_load( Elf32_Ehdr *elf, int nameserver );
int  elf_load_file( const char *name, int nameserver );

static void  bss_init( void );
static void  framebuffer_init( void );
static void *allot_pages( unsigned pages );
static void *allot_stack( unsigned pages );
static void *stack_push( unsigned *stack, unsigned foo );

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

				fb[index] = ((y & 0xff) << 16) | ((x & 0xff) << 8) | x ^ y;
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

static void *allot_stack( unsigned pages ){
	uint8_t *ret = allot_pages( pages );

	ret += pages * PAGE_SIZE - 4;

	return ret;
}

static void *stack_push( unsigned *stack, unsigned foo ){
	*(stack--) = foo;

	return stack;
}

static inline void elf_load_set_arg( uint8_t *stack,
                                     unsigned offset,
                                     unsigned arg,
                                     unsigned value )
{
	*((unsigned *)(stack + offset) + arg + 1) = value;
}

static struct foo *forth_sysinfo;

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

void test_thread( void *data ){
	message_t msg;
	volatile struct foo *meh = data;

	msg.type = 0xcafe;

	for (;;){
		c4_msg_recieve( &msg, 0 );
		c4_msg_send( &msg, meh->target );
	}
}

extern const char *foo;

static inline bool is_page_fault( const message_t *msg ){
	return msg->type == MESSAGE_TYPE_PAGE_FAULT;
}

static inline bool is_keystroke( const message_t *msg ){
	return msg->type == 0xbeef;
}

void server( void *data ){
	message_t msg;
	struct foo *meh = data;

	while ( true ){
		c4_msg_recieve( &msg, 0 );

		if ( is_page_fault( &msg )){
			debug_print( meh, "got a page fault message, eh\n" );

		} else {
			debug_print( meh, "sigma0: got an unknown message, ignoring\n" );
		}
	}

	for ( ;; );
}

enum {
	CODE_ESCAPE,
	CODE_TAB,
	CODE_LEFT_CONTROL,
	CODE_RIGHT_CONTROL,
	CODE_LEFT_SHIFT,
	CODE_RIGHT_SHIFT,
};

const char lowercase[] =
	{ '`', CODE_ESCAPE, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-',
	  '=', '\b', CODE_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
	  '[', ']', '\n', CODE_LEFT_CONTROL, 'a', 's', 'd', 'f', 'g', 'h', 'j',
	  'k', 'l', ';', '\'', '?', CODE_LEFT_SHIFT, '?', 'z', 'x', 'c', 'v', 'b',
	  'n', 'm', ',', '.', '/', CODE_RIGHT_SHIFT, '_', '_', ' ', '_', '_', '_',
	  '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
	};

const char uppercase[] =
	{ '~', CODE_ESCAPE, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
	  '+', '\b', CODE_TAB, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
	  '{', '}', '\n', CODE_LEFT_CONTROL, 'A', 'S', 'D', 'F', 'G', 'H', 'J',
	  'K', 'L', ':', '"', '?', CODE_LEFT_SHIFT, '?', 'Z', 'X', 'C', 'V', 'B',
	  'N', 'M', '<', '>', '?', CODE_RIGHT_SHIFT, '_', '_', ' ', '_', '_', '_',
	  '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
	};

char decode_scancode( unsigned long code ){
	static bool is_uppercase = false;
	char c = is_uppercase? uppercase[code] : lowercase[code];
	char ret = '\0';

	switch ( c ){
		case CODE_LEFT_SHIFT:
		case CODE_RIGHT_SHIFT:
			is_uppercase = !is_uppercase;
			break;

		default:
			ret = c;
			break;
	}

	return ret;
}

void debug_putchar( char c ){
	message_t msg;

	msg.data[0] = c;
	msg.type    = MESSAGE_TYPE_DEBUG_PUTCHAR;

	c4_msg_send( &msg, 0 );
}

void debug_putstr( char *str ){
	for ( unsigned i = 0; str[i]; i++ ){
		debug_putchar( str[i] );
	}
}

void debug_print( struct foo *info, char *str ){
	debug_putstr( "--- sigma0: " );
	debug_putstr( str );
	debug_putchar( '\n' );
}

int c4_msg_send( message_t *buffer, unsigned to ){
	int ret = 0;

	DO_SYSCALL( SYSCALL_SEND, buffer, to, 0, 0, ret );

	return ret;
}

int c4_msg_recieve( message_t *buffer, unsigned from ){
	int ret = 0;

	DO_SYSCALL( SYSCALL_RECIEVE, buffer, from, 0, 0, ret );

	return ret;
}

int c4_msg_send_async( message_t *buffer, unsigned to ){
	int ret = 0;

	DO_SYSCALL( SYSCALL_SEND_ASYNC, buffer, to, 0, 0, ret );

	return ret;
}

int c4_msg_recieve_async( message_t *buffer, unsigned flags ){
	int ret = 0;

	DO_SYSCALL( SYSCALL_RECIEVE_ASYNC, buffer, flags, 0, 0, ret );

	return ret;
}

int c4_create_thread( void *entry, void *stack, unsigned flags ){
	int ret = 0;

	DO_SYSCALL( SYSCALL_CREATE_THREAD, entry, stack, flags, 0, ret );

	return ret;
}

int c4_continue_thread( unsigned thread ){
	message_t buf = { .type = MESSAGE_TYPE_CONTINUE, };

	return c4_msg_send( &buf, thread );
}

int c4_set_pager( unsigned thread, unsigned pager ){
	message_t buf = {
		.type = MESSAGE_TYPE_SET_PAGER,
		.data = { pager },
	};

	return c4_msg_send( &buf, thread );
}

void *c4_request_physical( uintptr_t virt,
                           uintptr_t physical,
                           unsigned size,
                           unsigned permissions )
{
	message_t msg = {
		.type = MESSAGE_TYPE_REQUEST_PHYS,
		.data = {
			virt,
			physical,
			size,
			permissions
		},
	};

	c4_msg_send( &msg, 0 );

	return (void *)virt;
}

int c4_mem_map_to( unsigned thread_id,
                   void *from,
                   void *to,
                   unsigned size,
                   unsigned permissions )
{
	message_t msg = {
		.type = MESSAGE_TYPE_MAP_TO,
		.data = {
			(uintptr_t)from,
			(uintptr_t)to,
			(uintptr_t)size,
			(uintptr_t)permissions,
		},
	};

	return c4_msg_send( &msg, thread_id );
}

int c4_mem_grant_to( unsigned thread_id,
                     void *from,
                     void *to,
                     unsigned size,
                     unsigned permissions )
{
	message_t msg = {
		.type = MESSAGE_TYPE_GRANT_TO,
		.data = {
			(uintptr_t)from,
			(uintptr_t)to,
			(uintptr_t)size,
			(uintptr_t)permissions,
		},
	};

	return c4_msg_send( &msg, thread_id );
}
