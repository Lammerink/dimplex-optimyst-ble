import struct, sys

# usage: python3 parse_att.py <decrypted.pcap> <access_address_hex e.g. 5e9c546b>
PCAP = sys.argv[1]
aa_be = int(sys.argv[2], 16)
AA = struct.pack("<I", aa_be)   # access address, little-endian on air

ATT_OPS = {
 0x01:"ERROR_RSP",0x02:"MTU_REQ",0x03:"MTU_RSP",0x04:"FIND_INFO_REQ",0x05:"FIND_INFO_RSP",
 0x08:"READ_BY_TYPE_REQ",0x09:"READ_BY_TYPE_RSP",0x0a:"READ_REQ",0x0b:"READ_RSP",
 0x10:"READ_BY_GROUP_REQ",0x11:"READ_BY_GROUP_RSP",0x12:"WRITE_REQ",0x13:"WRITE_RSP",
 0x16:"PREP_WRITE_REQ",0x18:"EXEC_WRITE_REQ",0x1b:"HANDLE_VALUE_NTF",0x1d:"HANDLE_VALUE_IND",
 0x52:"WRITE_CMD",0x1e:"HANDLE_VALUE_CONF",
}

with open(PCAP,"rb") as f:
    data=f.read()

le = data[:4] in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1')
endian = "<" if le else ">"
off=24; fn=0; t0=None; rows=[]
while off+16<=len(data):
    ts_s,ts_us,caplen,_ = struct.unpack(endian+"IIII", data[off:off+16]); off+=16
    pkt=data[off:off+caplen]; off+=caplen; fn+=1
    t=ts_s+ts_us/1e6
    if t0 is None: t0=t
    i=pkt.find(AA)
    if i<0: continue
    p=pkt[i+4:]
    if len(p)<2: continue
    llid=p[0]&0x03; length=p[1]; payload=p[2:2+length]
    if llid not in (1,2) or len(payload)<4: continue
    l2len,cid=struct.unpack("<HH",payload[:4])
    if cid!=0x0004: continue
    att=payload[4:4+l2len]
    if not att: continue
    op=att[0]; opn=ATT_OPS.get(op,f"0x{op:02x}"); body=att[1:]
    info=""
    if op==0x0a and len(body)>=2: info=f"handle=0x{struct.unpack('<H',body[:2])[0]:04x}"
    elif op==0x0b: info=f"value={body.hex()}"
    elif op in (0x12,0x52,0x16,0x1b,0x1d) and len(body)>=2:
        info=f"handle=0x{struct.unpack('<H',body[:2])[0]:04x} value={body[2:].hex()}"
    else: info=f"data={body.hex()}"
    rows.append((fn,t-t0,opn,info))

print(f"# {len(rows)} ATT PDUs  (AA=0x{aa_be:08x})\n")
print("=== WRITES (button commands) ===")
for fn,dt,opn,info in rows:
    if opn in ("WRITE_REQ","WRITE_CMD"):
        print(f"  t={dt:6.2f}s  frame {fn:5d}  {opn:10s} {info}")
print("\n=== full ATT trace ===")
for fn,dt,opn,info in rows:
    print(f"t={dt:6.2f}s frame {fn:5d}  {opn:18s} {info}")
