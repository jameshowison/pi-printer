#!/usr/bin/env python3
"""Stamp a visible label at the top and bottom of every page in a PDF.

Usage: stamp-pdf.py INPUT OUTPUT LABEL
"""
import sys
import fitz  # PyMuPDF

input_path, output_path, label = sys.argv[1], sys.argv[2], sys.argv[3]

doc = fitz.open(input_path)
for page in doc:
    h = page.rect.height
    # Top label: 14pt from top edge
    page.insert_text(fitz.Point(28, 14), label, fontsize=12, color=(0, 0, 0), overlay=True)
    # Bottom label: 14pt from bottom edge (fitz y increases downward)
    page.insert_text(fitz.Point(28, h - 6), label, fontsize=12, color=(0, 0, 0), overlay=True)
doc.save(output_path)
