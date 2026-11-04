use std::{env, fs, io::{self, Read}, path::{Path, PathBuf}};

fn safe_path(root: &Path, entry: &str) -> Option<PathBuf> {
    let normalized = entry.replace('\\', "/");
    let relative = Path::new(&normalized);
    if relative.is_absolute() || relative.components().any(|c| matches!(c, std::path::Component::ParentDir)) {
        return None;
    }
    Some(root.join(relative))
}

fn main() -> io::Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: gma_extract <archive.gma> <output-dir>");
        std::process::exit(2);
    }
    let archive_path = Path::new(&args[1]);
    let output = Path::new(&args[2]);
    fs::create_dir_all(output)?;
    let mut archive = gma::open(archive_path)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
    let entry_count = archive.entries().count();
    let mut extracted = 0usize;
    for index in 0..entry_count {
        let entry = archive.entries().nth(index).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "GMA entry disappeared")
        })?;
        let name = entry.filename().to_string();
        let Some(destination) = safe_path(output, &name) else { continue };
        if let Some(parent) = destination.parent() { fs::create_dir_all(parent)?; }
        archive.read_entry(&entry, |_, reader| {
            let mut bytes = Vec::new();
            reader.read_to_end(&mut bytes)?;
            fs::write(&destination, bytes)?;
            Ok::<(), io::Error>(())
        }).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
        extracted += 1;
        if extracted % 1000 == 0 { println!("extracted {extracted}"); }
    }
    println!("extracted {extracted} files");
    Ok(())
}
