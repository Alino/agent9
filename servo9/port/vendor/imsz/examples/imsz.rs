use clap::Parser;
use imsz::imsz;

fn main() {
    #[derive(Parser, Debug)]
    #[clap(author, version, about, long_about =
        "The imsz library gets image sizes from \
         files, this is a demo application.")]
    struct Args {
        files: Vec<String>,
    }

    let args = Args::parse();

    if args.files.is_empty() {
        #[cfg(any(target_family="unix", target_family="windows"))]
        {
            match imsz(std::io::stdin()) {
                Ok(info) => println!("<stdin>: {}, {} x {}", info.format, info.width, info.height),
                Err(error) => eprintln!("<stdin>: {}", error)
            }
        }
    } else {
        for fname in &args.files {
            match imsz(fname) {
                Ok(info) => println!("{}: {}, {} x {}", fname, info.format, info.width, info.height),
                Err(error) => eprintln!("{}: {}", fname, error)
            }
        }
    }
}
