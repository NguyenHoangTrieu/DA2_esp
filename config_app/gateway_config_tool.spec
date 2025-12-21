# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec file for Gateway Config Tool
Usage: pyinstaller gateway_config_tool.spec
"""

block_cipher = None

a = Analysis(
    ['gateway_config_tool.py'],
    pathex=[],
    binaries=[],
    datas=[
        # Include config_protocol.py as data file (backup)
        ('config_protocol.py', '.'),
    ],
    hiddenimports=[
        'serial',
        'serial.tools',
        'serial.tools.list_ports',
        'config_protocol',
        'tkinter',
        'tkinter.ttk',
        'tkinter.scrolledtext',
        'tkinter.filedialog',
        'tkinter.messagebox',
        'json',
        'threading',
        'subprocess',
        'pathlib',
        'datetime',
        'copy',
        'traceback',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'matplotlib',
        'numpy',
        'pandas',
        'PIL',
        'cv2',
    ],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name='GatewayConfigTool',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,
    version_file=None,
)
