use std::{env, fs, io::{self, Read}, path::{Path, PathBuf}};
use valve_pak::VPK;

fn safe_path(root: &Path, entry: &str) -> Option<PathBuf> {
    let normalized = entry.replace('\\', "/");
    let relative = Path::new(&normalized);
    if relative.is_absolute()
        || relative.components().any(|c| matches!(c, std::path::Component::ParentDir)) {
        return None;
    }
    Some(root.join(relative))
}

fn main() -> io::Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: vpk_extract <output-dir> [--prefix=<path>...] <archive.vpk>...");
        eprintln!("  --prefix: extract every file whose path starts with it instead of the");
        eprintln!("            default VMT/VTF/model set (for example sound/player/footsteps/)");
        std::process::exit(2);
    }
    let output = Path::new(&args[1]);
    fs::create_dir_all(output)?;
    let prefixes: Vec<String> = args[2..].iter()
        .filter_map(|a| a.strip_prefix("--prefix=").map(|p| p.replace('\\', "/").to_ascii_lowercase()))
        .collect();
    let mut extracted = 0usize;
    for archive_path in args[2..].iter().filter(|a| !a.starts_with("--prefix=")) {
        let archive = VPK::open(archive_path)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
        for entry in archive.file_paths() {
            let lower = entry.to_ascii_lowercase().replace('\\', "/");
            let wanted = if prefixes.is_empty() {
                lower.ends_with(".vmt")
                    || lower.ends_with(".vtf")
                    || lower.ends_with(".mdl")
                    || lower.ends_with(".vvd")
                    || lower.ends_with(".vtx")
                    || lower.ends_with(".phy")
            } else {
                prefixes.iter().any(|p| lower.starts_with(p.as_str()))
            };
            if !wanted {
                continue;
            }
            let Some(destination) = safe_path(output, entry) else { continue };
            if let Some(parent) = destination.parent() { fs::create_dir_all(parent)?; }
            let mut reader = archive.get_file(entry)
                .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
            let mut bytes = Vec::new();
            reader.read_to_end(&mut bytes)?;
            fs::write(destination, bytes)?;
            extracted += 1;
        }
    }
    println!("extracted {extracted} files");
    Ok(())
}
