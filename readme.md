# http.c

> [!WARNING]
> This "code" is not something you should use ANYWHERE. It's BAD.

The simplest functional HTTP/1.1 server I could come up with. Built with bare minimum effort, for fun.

It was supposed to be able to host some static files, and I guess it works for that.

## Usage

It's supposed to be used like the python built-in http server:

```bash
httpc <ipv4-address>[:port, default=8080] [directory, default='.']
```

## Building

`ONLY LINUX IS SUPPORTED FOR NOW`

Compilation is so simple, that I didn't even add a MakeFile.

Just 

```bash
cc http.c -o httpc
```

Or whatever - I couldn't be bothered to complicate the build system to make it seem, like the project is bigger than it is.

## License

`party.gif` I've found [here](https://tenor.com/view/teto-fruits-teto-food-teto-foods-kasane-teto-teto-gif-12782683999665099064). I do not own that image.

Other than that, do whatever (see [LICENSE](LICENSE)).
