use std::{env, fs, io::{self, Read, Seek, SeekFrom}, path::{Path, PathBuf}};

const HEADER_SIZE: usize = 8 + 64 * 16;
const PAKFILE_LUMP: usize = 40;

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
    if args.len() != 3 {
        eprintln!("usage: bsp_pak_extract <map.bsp> <output-dir>");
        std::process::exit(2);
    }
    let mut file = fs::File::open(&args[1])?;
    let mut header = vec![0u8; HEADER_SIZE];
    file.read_exact(&mut header)?;
    if &header[0..4] != b"VBSP" {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "not a VBSP file"));
    }
    let lump_base = 8 + PAKFILE_LUMP * 16;
    let offset = i32::from_le_bytes(header[lump_base..lump_base + 4].try_into().unwrap());
    let length = i32::from_le_bytes(header[lump_base + 4..lump_base + 8].try_into().unwrap());
    if offset < 0 || length < 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid pakfile lump"));
    }
    file.seek(SeekFrom::Start(offset as u64))?;
    let mut pak = vec![0u8; length as usize];
    file.read_exact(&mut pak)?;
    let reader = std::io::Cursor::new(pak);
    let mut archive = zip::ZipArchive::new(reader)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
    let output = Path::new(&args[2]);
    fs::create_dir_all(output)?;
    let mut extracted = 0usize;
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e.to_string()))?;
        let name = entry.name().replace('\\', "/");
        let Some(destination) = safe_path(output, &name) else { continue };
        if entry.is_dir() {
            fs::create_dir_all(destination)?;
            continue;
        }
        if let Some(parent) = destination.parent() { fs::create_dir_all(parent)?; }
        let mut bytes = Vec::new();
        entry.read_to_end(&mut bytes)?;
        fs::write(destination, bytes)?;
        extracted += 1;
    }
    println!("extracted {extracted} BSP pakfile entries");
    Ok(())
}
