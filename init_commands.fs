: print-string ( addr -- )
  while dup c@ 0 != begin
      dup c@ emit
      1 +
  repeat drop
;

: hex ( n -- )
  "0x" print-string .x cr drop
;

: print-memfield ( n name -- )
  print-string ": " print-string . " cells (" print-string
  cells .  " bytes)" print-string
  cr drop
;

: meminfo
  push-meminfo
  "available memory:" print-string cr
  "     calls" print-memfield
  "parameters" print-memfield
  "      data" print-memfield
;

: make-msgbuf create 8 cells allot ;
: get-type    @ ;
: set-type    ! ;
: is-keycode? get-type 0xbabe = ;
: get-keycode 2 cells + @ ;

1  value debug-msg
2  value map-msg
3  value mapto-msg
4  value unmap-msg
5  value grant-msg
6  value grantto-msg
7  value requestphys-msg
8  value pagefault-msg
9  value dumpmaps-msg
10 value stop-msg
11 value continue-msg
12 value end-msg
13 value kill-msg

make-msgbuf buffer

: msgtest
  buffer recvmsg
  if buffer is-keycode? then
      "got a keypress: " print-string
      buffer get-keycode . cr drop
  else
      "got message with type " print-string
      buffer get-type . cr drop
  end
;

: do-send  buffer set-type buffer swap sendmsg ;
: stop     stop-msg     do-send ;
: continue continue-msg do-send ;
: debug    debug-msg    do-send ;
: kill     kill-msg     do-send ;
: dumpmaps dumpmaps-msg do-send ;
: ping     1234         do-send ;

: help
  "some things you can do:" print-string cr
  "  print-archives  : list available base words" print-string cr
  "  help            : print this help" print-string cr
  "  meminfo         : print the amount of memory available" print-string cr
  "  dumpmaps        : dump memory maps for the given thread" print-string cr

  "  [addr] [len] /x : interactive hex dump of [len] words at [addr]" print-string cr
  "    [id] continue : continue thread [id]" print-string cr
;

: initfs-print ( name -- )
  tarfind dup tarsize
  "size: " print-string .   cr drop
  "addr: " print-string hex cr
;

0xffffbeef value list-start
list-start value [[
: ]] ;

: dumpmaps-list
  while dup list-start != begin
    dumpmaps
  repeat
;

: ls ( -- )
  "/" tarfind
  while dup c@ 0 != begin
    dup print-string cr
    tarnext
  repeat
;

: exec ( program-path -- )
  "loading elf executable: " print-string dup print-string cr
  tarfind elfload drop
;

: exec-list ( path-list ... -- )
  while dup list-start != begin
    exec
  repeat
  drop
;

( initial program list to bootstrap the system )
: bootstrap ( -- )
  [[
    "./bin/keyboard"
    "./bin/test"
  ]] exec-list
;

( Hex dumper definitions )
: hex-len ( n -- len )
  if dup 0 = then drop 1 end

  0 swap while dup 0 > begin
    0x10 /
    swap 1 + swap
  repeat drop
;

: hex-pad ( n -- )
  hex-len 8 swap - while dup 0 > begin
    0x30 emit
    1 -
  repeat drop
;

: /x-hex-label swap dup hex-pad .x swap 0x20 0x3a emit emit ;
: /x-key       buffer recvmsg ;

: /x-emitchars ( addr -- )
  0 while dup 16 < begin
    swap
      if dup c@ 0x19 > then
        dup c@ emit
      else
        0x2e emit
      end
      1 +
    swap
    1 +
  repeat drop drop
;

: /x-chars
  0x7c emit over 4 cells - /x-emitchars 0x7c emit
;

: /x ( addr n -- )
  /x-hex-label

  while dup 0 > begin
    swap
      dup @ dup hex-pad .x drop
      1 cells +
    swap

    0x20 emit
    1 -

    if dup 96 mod 0 = then
      /x-chars
      cr 0x3a emit
      /x-key
      /x-hex-label

    else if dup 4 mod 0 = then
      /x-chars
      cr
      /x-hex-label
    end
    end

  repeat
  drop drop
;

bootstrap
"All systems are go, good luck" print-string cr

( XXX : newline needed at the end of the file because the init routine )
(       sets the last byte of the file to 0 )

