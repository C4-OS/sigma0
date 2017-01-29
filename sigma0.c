#include <sigma0/sigma0.h>
#include <sigma0/tar.h>
#include <sigma0/elf.h>
#include <miniforth/miniforth.h>
#include <c4/thread.h>
#include <c4/bootinfo.h>

struct foo {
	int target;
	int display;
	int forth;
	int nameserver;
};

static bootinfo_t *c4_bootinfo = (void *)0xfcfff000;

// external binaries linked into the image
// forth initial commands file
extern char _binary_init_commands_fs_start[];
extern char _binary_init_commands_fs_end[];
// tar
extern char _binary_initfs_tar_start[];
extern char _binary_initfs_tar_end[];

static tar_header_t *tar_initfs = (void *)_binary_initfs_tar_start;

int c4_set_pager( unsigned thread, unsigned pager );

void test_thread( void *unused );
void forth_thread( void *sysinfo );
void debug_putchar( char c );
void debug_print( struct foo *info, char *asdf );
int  elf_load( Elf32_Ehdr *elf, int nameserver );
int  elf_load_file( const char *name, int nameserver );

static void  bss_init( void );
static void *allot_pages( unsigned pages );
static void *allot_stack( unsigned pages );
static void *stack_push( unsigned *stack, unsigned foo );

void main( void ){
	struct foo thing;

	// set the memory in bss to zero, which is needed since this loaded
	// as a flat binary
	bss_init( );

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

	unsigned *s = allot_stack( 1 );
	s -= 1;
	thing.display = c4_create_thread( display_thread, s, 0 );

	s = allot_stack( 2 );
	s = stack_push( s, (unsigned)&thing );

	thing.forth = c4_create_thread( forth_thread, s, 0 );

	c4_set_pager( thing.display, 1 );
	c4_set_pager( thing.forth, 1 );

	c4_continue_thread( thing.display );
	c4_continue_thread( thing.forth );

	elf_load_file( "./bin/nameserver", 3 );
	elf_load_file( "./bin/pci", 3 );

	server( &thing );

	// TODO: panic or dump debug info or something, server()
	//       should never return
	for ( ;; );
}

static void bss_init( void ){
	extern uint8_t *bss_start;
	extern uint8_t *bss_end;

	for ( uint8_t *ptr = bss_start; ptr < bss_end; ptr++ ){
		*ptr = 0;
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

	return 0;
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

enum {
	NAME_BIND = 0x1024,
	NAME_UNBIND,
	NAME_LOOKUP,
	NAME_RESULT,
};

unsigned hash_string( const char *str ){
	unsigned hash = 757;
	int c;

	while (( c = *str++ )){
		hash = ((hash << 7) + hash + c);
	}

	return hash;
}

static inline unsigned nameserver_lookup( unsigned server, unsigned long name ){
	message_t msg = {
		.type = NAME_LOOKUP,
		.data = { name },
	};

	c4_msg_send( &msg, server );
	c4_msg_recieve( &msg, server );

	return msg.data[0];
}

static char *read_line( char *buf, unsigned n ){
	message_t msg;
	unsigned i = 0;

	for ( i = 0; i < n - 1; i++ ){
		char c = 0;
retry:
		{
			// XXX: this relies on the /bin/keyboard program being started,
			//      which itself is initialized by the forth interpreter,
			//      so this assumes that the program will be started by the
			//      init_commands.fs script before entering interactive mode
			static unsigned keyboard_thread = 0;

			if ( !keyboard_thread ){
				keyboard_thread =
					nameserver_lookup( 5, hash_string( "/dev/keyboard" ));
				goto retry;
			}

			msg.type = 0xbadbeef;
			c4_msg_send( &msg, keyboard_thread );
			c4_msg_recieve( &msg, keyboard_thread );

			c = decode_scancode( msg.data[0] );

			if ( c && msg.data[1] == 0 ){
				msg.type    = 0xbabe;
				msg.data[0] = c;

			} else {
				goto retry;
			}
		}

		c4_msg_send( &msg, forth_sysinfo->display );

		if ( i && c == '\b' ){
			i--;
			goto retry;
		}

		buf[i] = c;

		if ( c == '\n' ){
			break;
		}
	}

	buf[++i] = '\0';

	return buf;
}

char minift_get_char( void ){
	static char input[80];
	static bool initialized = false;
	static char *ptr;

	if ( !initialized ){
		*(_binary_init_commands_fs_end - 1) = 0;

		for ( unsigned i = 0; i < sizeof(input); i++ ){ input[i] = 0; }
		ptr         = _binary_init_commands_fs_start;
		initialized = true;
	}

	while ( !*ptr ){
		//debug_print( forth_sysinfo, "miniforth > " );
		ptr = read_line( input, sizeof( input ));
	}

	return *ptr++;
}

void minift_put_char( char c ){
	/*
	message_t msg;

	msg.type    = 0xbabe;
	msg.data[0] = c;

	c4_msg_send( &msg, forth_sysinfo->display );
	*/
	debug_putchar( c );
}

static bool c4_minift_sendmsg( minift_vm_t *vm );
static bool c4_minift_recvmsg( minift_vm_t *vm );
static bool c4_minift_tarfind( minift_vm_t *vm );
static bool c4_minift_tarsize( minift_vm_t *vm );
static bool c4_minift_tarnext( minift_vm_t *vm );
static bool c4_minift_elfload( minift_vm_t *vm );

static minift_archive_entry_t c4_words[] = {
	{ "sendmsg", c4_minift_sendmsg, 0 },
	{ "recvmsg", c4_minift_recvmsg, 0 },
	{ "tarfind", c4_minift_tarfind, 0 },
	{ "tarsize", c4_minift_tarsize, 0 },
	{ "tarnext", c4_minift_tarnext, 0 },
	{ "elfload", c4_minift_elfload, 0 },
};

void forth_thread( void *sysinfo ){
	forth_sysinfo = sysinfo;

	volatile unsigned x = forth_sysinfo->display;

	unsigned long data[1024 + 512];
	unsigned long calls[32];
	unsigned long params[32];

	minift_vm_t foo;
	minift_archive_t arc = {
		.name    = "c4",
		.entries = c4_words,
		.size    = sizeof(c4_words) / sizeof(minift_archive_entry_t),
	};

	for ( ;; ){
		minift_stack_t data_stack = {
			.start = data,
			.ptr   = data,
			.end   = data + 1536,
		};

		minift_stack_t call_stack = {
			.start = calls,
			.ptr   = calls,
			.end   = calls + 32,
		};

		minift_stack_t param_stack = {
			.start = params,
			.ptr   = params,
			.end   = params + 32,
		};

		minift_init_vm( &foo, &call_stack, &data_stack, &param_stack, NULL );
		minift_archive_add( &foo, &arc );
		minift_run( &foo );
		debug_print( sysinfo, "forth vm exited, restarting...\n" );
	}
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


static bool c4_minift_sendmsg( minift_vm_t *vm ){
	unsigned long target = minift_pop( vm, &vm->param_stack );
	unsigned long temp   = minift_pop( vm, &vm->param_stack );
	message_t *msg = (void *)temp;

	if ( !vm->running ){
		return false;
	}

	c4_msg_send( msg, target );

	return true;
}

static bool c4_minift_recvmsg( minift_vm_t *vm ){
	//  TODO: add 'from' argument, once that's supported
	unsigned long temp   = minift_pop( vm, &vm->param_stack );
	message_t *msg = (void *)temp;

	if ( !vm->running ){
		return false;
	}

	c4_msg_recieve( msg, 0 );

	return true;
}

static bool c4_minift_tarfind( minift_vm_t *vm ){
	const char   *name   = (const char *)minift_pop( vm, &vm->param_stack );

	// simulate a "root" directory, which just returns the start of the
	// tar archive
	if ( name[0] == '/' && name[1] == '\0' ){
		minift_push( vm, &vm->param_stack, (unsigned long)tar_initfs );

	} else {
		tar_header_t *lookup = tar_lookup( tar_initfs, name );
		minift_push( vm, &vm->param_stack, (unsigned long)lookup );
	}

	return true;
}

static bool c4_minift_tarsize( minift_vm_t *vm ){
	tar_header_t *lookup = (tar_header_t *)minift_pop( vm, &vm->param_stack );

	if ( lookup ){
		minift_push( vm, &vm->param_stack, tar_data_size( lookup ));

	} else {
		minift_push( vm, &vm->param_stack, 0 );
	}

	return true;
}

static bool c4_minift_tarnext( minift_vm_t *vm ){
	tar_header_t *temp = (tar_header_t *)minift_pop( vm, &vm->param_stack );

	temp = tar_next( temp );
	minift_push( vm, &vm->param_stack, (unsigned long)temp );

	return true;
}

static bool c4_minift_elfload( minift_vm_t *vm ){
	tar_header_t *temp = (tar_header_t *)minift_pop( vm, &vm->param_stack );

	void *data = tar_data( temp );
	// TODO: change this once a generic structure for passing info to new
	//       threads is implemented
	int id = elf_load( data, 5 );

	minift_push( vm, &vm->param_stack, id );
	return true;
}
