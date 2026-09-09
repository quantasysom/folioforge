# Third-party dependencies

This development build consumes installed Qt, QPDF, PDFium, zlib, and libjpeg-turbo artifacts. It does not download or rebuild them. The local deployment step copies available PDFium/QPDF/zlib/libjpeg-turbo notices beside the executable. Qt deployment can bring additional plugins and modules with their own obligations.

Installed notice locations:

- Qt: `C:/Qt/Licenses` and the installed Qt distribution's licensing materials.
- QPDF: `C:/projects/vPDF/vcpkg_installed/x64-windows/share/qpdf/copyright`.
- zlib: `C:/projects/vPDF/vcpkg_installed/x64-windows/share/zlib/copyright`.
- libjpeg-turbo: `C:/projects/vPDF/vcpkg_installed/x64-windows/share/libjpeg-turbo/copyright`.
- PDFium: `C:/projects/vPDF/third_party/pdfium/LICENSE` and its `licenses/` directory.

The fixture PDFs are generated from original test instructions using a standard Helvetica font reference; they embed no third-party font file, customer data, or private document. Desktop UI fonts are supplied by the operating system, not bundled by this project. The architecture/mockup were supplied in the workspace and their redistribution rights have not been independently reviewed.

The ASCII Helvetica/Helvetica-Bold advance tables in `engine/src/text_edit.cpp` contain standard numeric PDF font metrics, checked against the installed ReportLab `pdfmetrics` data. Courier uses its standard fixed 600-unit advance. No font program or ReportLab implementation code is bundled. Metric-compatible primary reference: [Artifex Nimbus Sans AFM](https://github.com/ArtifexSoftware/urw-base35-fonts/blob/master/fonts/NimbusSans-Regular.afm).

The project's license is Apache-2.0 (see LICENSE). Before public distribution, complete an inventory of the exact deployed modules, third-party sources, notices, source-offer/relinking requirements, fonts, and packaging assets. Available notices and top-level dependency labels alone do not establish release compliance.
