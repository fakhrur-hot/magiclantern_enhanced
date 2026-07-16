path = '/mnt/c/Users/Public/Kiro/ML_6D/source/modules/Makefile'
with open(path, 'rb') as f:
    raw = f.read()

# Need yolo line to end with backslash for continuation
# Current: b'    yolo \n    tcc'
# Needed:  b'    yolo \\\n    tcc'  (single backslash before newline)
old = b'    yolo \n    tcc'
new = b'    yolo \\\n    tcc'
assert old in raw, "pattern not found"
raw2 = raw.replace(old, new)

with open(path, 'wb') as f:
    f.write(raw2)

idx = raw2.find(b'    yolo')
print('fixed:', repr(raw2[idx:idx+22]))
