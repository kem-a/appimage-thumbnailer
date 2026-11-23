//! Renders icon bytes (raster or SVG) into a square PNG thumbnail, with a
//! soft drop shadow when the `shadow` feature is on.

use std::path::Path;

use image::imageops::FilterType;
use resvg::tiny_skia::{Pixmap, Transform};
use resvg::usvg;
use resvg::usvg::fontdb::{Database, Family};

/// Drop shadow geometry for one thumbnail size. All zero without the `shadow` feature.
#[derive(Debug, Default, PartialEq)]
struct Shadow {
    blur_radius: u32,
    passes: u32,
    offset_y: u32,
    opacity: f32,
    /// Per-side margins. The shadow falls downward, so the bottom one is larger.
    top: u32,
    bottom: u32,
    left: u32,
    right: u32,
}

fn shadow(size: u32) -> Shadow {
    if !cfg!(feature = "shadow") {
        return Shadow::default();
    }
    let blur_radius = (size / 64).max(2);
    let passes = 3; // 3 box blurs approximate a Gaussian
    let offset_y = (size / 32).max(2);
    // Reserve the blur's full reach so no tail clips, and the offset only at the bottom.
    let reach = blur_radius * passes;
    let pad = (size / 256).max(1);
    // Never let a side take more than a quarter of the frame.
    let cap = (size / 4).max(1);
    Shadow {
        blur_radius,
        passes,
        offset_y,
        opacity: 0.55,
        top: (reach.saturating_sub(offset_y) + pad).min(cap),
        bottom: (reach + offset_y + pad).min(cap),
        left: (reach + pad).min(cap),
        right: (reach + pad).min(cap),
    }
}

/// The box the icon is fitted into, as (x, y, width, height).
type IconBox = (u32, u32, u32, u32);

fn icon_box(s: &Shadow, size: u32) -> IconBox {
    let width = size.saturating_sub(s.left + s.right).max(1);
    let height = size.saturating_sub(s.top + s.bottom).max(1);
    (s.left, s.top, width, height)
}

pub fn write_thumbnail(data: &[u8], size: u32, out: &Path) -> Result<(), String> {
    let shadow = shadow(size);
    let bbox = icon_box(&shadow, size);
    let mut icon = Pixmap::new(size, size).ok_or("Failed to allocate image surface")?;
    match image::guess_format(data) {
        Ok(format) => paint_raster(&mut icon, data, format, bbox)?,
        Err(_) => paint_svg(&mut icon, data, bbox)?,
    }
    if cfg!(feature = "shadow") {
        add_shadow(&mut icon, &shadow);
    }
    icon.save_png(out)
        .map_err(|e| format!("Failed to write thumbnail: {e}"))?;
    debug!("write_thumbnail: wrote '{}'", out.display());
    Ok(())
}

fn paint_raster(
    dst: &mut Pixmap,
    data: &[u8],
    format: image::ImageFormat,
    (bx, by, bw, bh): IconBox,
) -> Result<(), String> {
    let mut img = image::load_from_memory_with_format(data, format)
        .map_err(|e| format!("Failed to decode {format:?} icon: {e}"))?
        .into_rgba8();
    let (w, h) = img.dimensions();
    debug!("paint_raster: {format:?} {w}x{h}");

    // Pixmaps hold premultiplied RGBA. Premultiply before scaling so the
    // color of transparent pixels does not bleed into the edges.
    for p in img.pixels_mut() {
        let a = u32::from(p[3]);
        for c in &mut p.0[..3] {
            *c = ((u32::from(*c) * a + 127) / 255) as u8;
        }
    }

    let fit = bw.min(bh);
    let scale = (f64::from(fit) / f64::from(w)).min(f64::from(fit) / f64::from(h));
    let tw = ((f64::from(w) * scale).round() as u32).clamp(1, fit);
    let th = ((f64::from(h) * scale).round() as u32).clamp(1, fit);
    if (tw, th) != (w, h) {
        img = image::imageops::resize(&img, tw, th, FilterType::Triangle);
    }

    // Center in the box, clipped to the canvas (the box can overhang at size 1).
    let size = dst.width() as usize;
    let ox = (bx + (bw - tw) / 2) as usize;
    let oy = (by + (bh - th) / 2) as usize;
    let canvas = dst.data_mut();
    for (y, row) in img.rows().enumerate().take(size.saturating_sub(oy)) {
        for (x, p) in row.enumerate().take(size.saturating_sub(ox)) {
            let i = ((oy + y) * size + ox + x) * 4;
            canvas[i..i + 4].copy_from_slice(&p.0);
        }
    }
    Ok(())
}

fn paint_svg(dst: &mut Pixmap, data: &[u8], (bx, by, bw, bh): IconBox) -> Result<(), String> {
    let mut options = usvg::Options::default();
    // Fonts are only needed for <text>, and loading them is slow.
    if data.windows(5).any(|w| w == b"<text") {
        let db = options.fontdb_mut();
        db.load_system_fonts();
        use_installed_generic_families(db);
    }
    let tree = usvg::Tree::from_data(data, &options)
        .map_err(|e| format!("Unsupported icon, neither a known raster format nor SVG: {e}"))?;

    let (w, h) = (tree.size().width(), tree.size().height());
    let scale = (bw as f32 / w).min(bh as f32 / h);
    let tx = bx as f32 + (bw as f32 - w * scale) / 2.0;
    let ty = by as f32 + (bh as f32 - h * scale) / 2.0;
    debug!("paint_svg: {w}x{h}, scale {scale}");
    resvg::render(
        &tree,
        Transform::from_row(scale, 0.0, 0.0, scale, tx, ty),
        &mut dst.as_mut(),
    );
    Ok(())
}

/// fontdb maps a generic family to the first fontconfig alias even when that
/// font is not installed (FreeSans on Fedora), and text in it is dropped.
/// Switch such a family to the first installed common font.
fn use_installed_generic_families(db: &mut Database) {
    fn installed(db: &Database, name: &str) -> bool {
        db.faces()
            .any(|face| face.families.iter().any(|(family, _)| family == name))
    }
    let choices: [(Family, &[&str]); 3] = [
        (
            Family::SansSerif,
            &[
                "Noto Sans",
                "DejaVu Sans",
                "Liberation Sans",
                "Cantarell",
                "Arial",
            ],
        ),
        (
            Family::Serif,
            &[
                "Noto Serif",
                "DejaVu Serif",
                "Liberation Serif",
                "Times New Roman",
            ],
        ),
        (
            Family::Monospace,
            &[
                "Noto Sans Mono",
                "DejaVu Sans Mono",
                "Liberation Mono",
                "Courier New",
            ],
        ),
    ];
    for (family, names) in choices {
        if installed(db, db.family_name(&family)) {
            continue;
        }
        if let Some(&name) = names.iter().find(|name| installed(db, name)) {
            debug!(
                "use_installed_generic_families: {} -> {name}",
                db.family_name(&family)
            );
            match family {
                Family::Serif => db.set_serif_family(name),
                Family::Monospace => db.set_monospace_family(name),
                _ => db.set_sans_serif_family(name),
            }
        }
    }
}

/// Puts a blurred, offset copy of the icon's alpha beneath the icon.
fn add_shadow(icon: &mut Pixmap, s: &Shadow) {
    let n = icon.width() as usize;
    let mut mask: Vec<u8> = icon
        .data()
        .as_chunks::<4>()
        .0
        .iter()
        .map(|p| p[3])
        .collect();
    box_blur(&mut mask, n, s.blur_radius as usize, s.passes);

    let dy = s.offset_y as usize;
    for (i, px) in icon
        .data_mut()
        .as_chunks_mut::<4>()
        .0
        .iter_mut()
        .enumerate()
    {
        let (x, y) = (i % n, i / n);
        let m = if y >= dy { mask[(y - dy) * n + x] } else { 0 };
        let shadow = (f32::from(m) * s.opacity).round() as u32;
        // Premultiplied icon over black: colors stay, alpha accumulates.
        let a = u32::from(px[3]);
        px[3] = (a + (shadow * (255 - a) + 127) / 255) as u8;
    }
}

/// Separable box blur of an n x n 8-bit buffer, in place.
fn box_blur(data: &mut [u8], n: usize, radius: usize, passes: u32) {
    if radius == 0 || n == 0 {
        return;
    }
    let mut tmp = vec![0u8; n * n];
    for _ in 0..passes {
        blur_lines(data, &mut tmp, n, n, 1, radius); // rows
        blur_lines(&tmp, data, n, 1, n, radius); // columns
    }
}

/// One box blur over n lines of n samples. A line starts at `line * line_step`
/// and its samples are `step` apart. Edge samples repeat past the border.
fn blur_lines(src: &[u8], dst: &mut [u8], n: usize, line_step: usize, step: usize, radius: usize) {
    let window = (2 * radius + 1) as u32;
    let r = radius as isize;
    let at = |line: usize, i: isize| line * line_step + i.clamp(0, n as isize - 1) as usize * step;
    for line in 0..n {
        let mut sum: u32 = (-r..=r).map(|i| u32::from(src[at(line, i)])).sum();
        for i in 0..n as isize {
            dst[at(line, i)] = (sum / window) as u8;
            sum += u32::from(src[at(line, i + r + 1)]);
            sum -= u32::from(src[at(line, i - r)]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{Rgba, RgbaImage};
    use std::io::Cursor;

    fn png(w: u32, h: u32, color: [u8; 4]) -> Vec<u8> {
        let mut bytes = Vec::new();
        RgbaImage::from_pixel(w, h, Rgba(color))
            .write_to(&mut Cursor::new(&mut bytes), image::ImageFormat::Png)
            .unwrap();
        bytes
    }

    fn render(data: &[u8], size: u32) -> Result<RgbaImage, String> {
        let dir = tempfile::tempdir().unwrap();
        let out = dir.path().join("out.png");
        write_thumbnail(data, size, &out)?;
        Ok(image::open(out).unwrap().into_rgba8())
    }

    #[test]
    fn shadow_geometry() {
        let s = shadow(256);
        if cfg!(feature = "shadow") {
            assert_eq!((s.blur_radius, s.passes, s.offset_y), (4, 3, 8));
            assert_eq!((s.top, s.bottom, s.left, s.right), (5, 21, 13, 13));
            assert_eq!(icon_box(&s, 256), (13, 5, 230, 230));
            assert_eq!(icon_box(&shadow(1), 1), (1, 1, 1, 1));
        } else {
            assert_eq!(s, Shadow::default());
            assert_eq!(icon_box(&s, 256), (0, 0, 256, 256));
        }
    }

    #[test]
    fn box_blur_spreads_and_keeps_flat_areas() {
        let mut flat = vec![200u8; 9 * 9];
        box_blur(&mut flat, 9, 2, 3);
        assert!(flat.iter().all(|&v| v == 200));

        let mut dot = vec![0u8; 21 * 21];
        dot[10 * 21 + 10] = 255;
        box_blur(&mut dot, 21, 2, 1);
        assert_eq!(dot[10 * 21 + 10], 10);
        assert_eq!(dot.iter().map(|&v| u32::from(v)).sum::<u32>(), 25 * 10);
    }

    #[test]
    fn raster_icon_is_fitted_and_centered() {
        let out = render(&png(10, 20, [255, 0, 0, 255]), 64).unwrap();
        assert_eq!(out.dimensions(), (64, 64));
        let (bx, by, bw, bh) = icon_box(&shadow(64), 64);
        assert_eq!(
            *out.get_pixel(bx + bw / 2, by + bh / 2),
            Rgba([255, 0, 0, 255])
        );
        // 10x20 scaled to the box height leaves the left part of the box empty.
        assert_eq!(out.get_pixel(bx + 1, by + bh / 2)[3], 0);
        assert_eq!(out.get_pixel(0, 0)[3], 0);
    }

    #[test]
    fn svg_icon() {
        let svg = br##"<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
            <rect width="8" height="8" fill="#00ff00"/></svg>"##;
        let out = render(svg, 64).unwrap();
        assert_eq!(*out.get_pixel(32, 32), Rgba([0, 255, 0, 255]));
    }

    #[test]
    fn svg_text_with_generic_family() {
        let mut db = Database::new();
        db.load_system_fonts();
        use_installed_generic_families(&mut db);
        let sans = db.family_name(&Family::SansSerif).to_owned();
        if !db
            .faces()
            .any(|face| face.families.iter().any(|(family, _)| *family == sans))
        {
            eprintln!("no sans-serif font installed, skipping");
            return;
        }
        let svg = br#"<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
            <text x="4" y="44" font-family="sans-serif" font-size="40">Ab</text></svg>"#;
        let out = render(svg, 64).unwrap();
        assert!(out.pixels().any(|p| p[3] == 255));
    }

    #[test]
    fn shadow_falls_below_the_icon() {
        let out = render(&png(4, 4, [255, 255, 255, 255]), 128).unwrap();
        let (_, by, _, bh) = icon_box(&shadow(128), 128);
        if cfg!(feature = "shadow") {
            let below = out.get_pixel(64, by + bh + 1);
            assert!(below[3] > 0 && below[0] == 0, "{below:?}");
        } else {
            assert_eq!(out.get_pixel(64, 127)[3], 255); // the icon fills the frame
        }
    }

    #[test]
    fn tiny_sizes_and_bad_data() {
        for size in [1, 2, 3, 7] {
            assert_eq!(
                render(&png(5, 5, [1, 2, 3, 255]), size).unwrap().width(),
                size
            );
        }
        assert!(render(b"not an image", 64).is_err());
        assert!(render(&png(5, 5, [0, 0, 0, 255])[..30], 64).is_err());
    }
}
