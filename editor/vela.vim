" Vim syntax file for Vela.
" Install: cp editor/vela.vim ~/.vim/syntax/vela.vim
"          echo 'au BufRead,BufNewFile *.vela set filetype=vela' >> ~/.vimrc

if exists("b:current_syntax") | finish | endif

syn keyword velaKeyword     fn let mut const type use as return
syn keyword velaConditional if else match
syn keyword velaRepeat      while for in break continue
syn keyword velaStructure   struct enum pub test
syn keyword velaOperator    and or not
syn keyword velaBoolean     true false nil
syn keyword velaSelf        self
syn keyword velaType        Int Float Bool Byte Str Void Range Error List Map
syn keyword velaBuiltin     str int float byte bool len print println panic
syn keyword velaBuiltin     assert assert_eq assert_ne ok err err_code void

syn match   velaIntrinsic   "@\w\+"
syn match   velaNumber      "\<\d[0-9_]*\>"
syn match   velaNumber      "\<0[xX][0-9a-fA-F_]\+\>"
syn match   velaNumber      "\<0[bB][01_]\+\>"
syn match   velaNumber      "\<0[oO][0-7_]\+\>"
syn match   velaFloat       "\<\d[0-9_]*\.\d[0-9_]*\([eE][-+]\?\d\+\)\?\>"
syn match   velaFloat       "\<\d[0-9_]*[eE][-+]\?\d\+\>"
syn match   velaChar        "'\([^'\\]\|\\.\)'"
syn match   velaFunction    "\<fn\s\+\zs\w\+"
syn match   velaMethod      "\.\zs\w\+\ze("

syn region  velaString      start=+"+ skip=+\\.+ end=+"+ contains=velaEscape,velaInterp
syn match   velaEscape      contained "\\[nrt0\\\"'{}]\|\\x[0-9a-fA-F]\{2}"
syn region  velaInterp      contained matchgroup=velaInterpDelim start="{" end="}" contains=ALLBUT,velaInterp

syn region  velaComment     start="//" end="$" contains=velaTodo
syn region  velaDoc         start="///" end="$" contains=velaTodo
syn region  velaBlockComment start="/\*" end="\*/" contains=velaBlockComment,velaTodo
syn keyword velaTodo        contained TODO FIXME NOTE XXX

hi def link velaKeyword      Keyword
hi def link velaConditional  Conditional
hi def link velaRepeat       Repeat
hi def link velaStructure    Structure
hi def link velaOperator     Operator
hi def link velaBoolean      Boolean
hi def link velaSelf         Identifier
hi def link velaType         Type
hi def link velaBuiltin      Function
hi def link velaIntrinsic    PreProc
hi def link velaNumber       Number
hi def link velaFloat        Float
hi def link velaChar         Character
hi def link velaString       String
hi def link velaEscape       SpecialChar
hi def link velaInterpDelim  SpecialChar
hi def link velaComment      Comment
hi def link velaDoc          SpecialComment
hi def link velaBlockComment Comment
hi def link velaTodo         Todo
hi def link velaFunction     Function
hi def link velaMethod       Function

let b:current_syntax = "vela"
