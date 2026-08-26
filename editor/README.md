# Editor support

## Syntax highlighting

**Vim / Neovim**

```console
$ mkdir -p ~/.vim/syntax && cp editor/vela.vim ~/.vim/syntax/
$ echo 'au BufRead,BufNewFile *.vela set filetype=vela' >> ~/.vimrc
```

**VS Code, Sublime, Zed, and anything else that reads TextMate grammars**

`editor/vela.tmLanguage.json` is a complete grammar with correct highlighting
inside string interpolations. For VS Code, put it in a small extension:

```json
{
  "name": "vela",
  "contributes": {
    "languages": [{ "id": "vela", "extensions": [".vela"] }],
    "grammars": [{
      "language": "vela",
      "scopeName": "source.vela",
      "path": "./vela.tmLanguage.json"
    }]
  }
}
```

## Language server

`vela-lsp` speaks the Language Server Protocol over stdin/stdout and provides:

* **diagnostics** on save and on change, from the real compiler
* **document formatting** and **range formatting**, from the real formatter
* **hover** with the declaration line and its doc comment
* **go to definition** within a file
* **document symbols** for the outline view

Build it:

```console
$ velac -o bin/vela-lsp tools/lsp.vela
```

Wire it up:

**Neovim**

```lua
vim.lsp.start({
  name = 'vela',
  cmd = { 'vela-lsp' },
  root_dir = vim.fs.dirname(vim.fs.find({ 'vela.toml' }, { upward = true })[1]),
})
```

**VS Code** — any generic LSP client extension pointed at the `vela-lsp` binary.

**Emacs (eglot)**

```elisp
(add-to-list 'eglot-server-programs '(vela-mode . ("vela-lsp")))
```

## Formatting on save

If your editor cannot run the language server, `vela fmt` is fast enough to run
on every save:

```vim
autocmd BufWritePost *.vela silent !vela fmt %
```
