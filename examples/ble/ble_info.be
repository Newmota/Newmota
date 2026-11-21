import BLE

class BLE_INFO
    var services

    def init(MAC,addr_type)
        import cb
        var cbp = cb.gen_cb(/e,o,u,h->self.cb(e,o,u,h))
        BLE.conn_cb(cbp)
        print("###########################################################################")
        print("#                Init NEWMOTA BLE device explorer                         #")
        print("###########################################################################")
        print(" ")
    end

    def get(MAC,addr_type)
        print("___________________________________________________________________________")
        print(f" Retrieve services and characteristics for BLE device wit MAC:{MAC} ")
        print("___________________________________________________________________________")
        BLE.set_MAC(bytes(MAC),addr_type)
        BLE.run(6)
    end

    def uuid128ToString(uuid)
        return uuid[0..3].tohex()+"-"+uuid[4..5].tohex()+"-"+uuid[6..7].tohex()+"-"+uuid[8..9].tohex()+"-"+uuid[10..15].tohex()
    end

    # BLE GATT properties:
    #define BLE_GATT_CHR_PROP_BROADCAST                     0x01
    #define BLE_GATT_CHR_PROP_READ                          0x02
    #define BLE_GATT_CHR_PROP_WRITE_NO_RSP                  0x04
    #define BLE_GATT_CHR_PROP_WRITE                         0x08
    #define BLE_GATT_CHR_PROP_NOTIFY                        0x10
    #define BLE_GATT_CHR_PROP_INDICATE                      0x20
    #define BLE_GATT_CHR_PROP_AUTH_SIGN_WRITE               0x40
    #define BLE_GATT_CHR_PROP_EXTENDED                      0x80

    def parseFlags(flags)
        var f = []
        if flags & 0x01 f.push("Broadcast") end
        if flags & 0x02 f.push("Read") end
        if flags & 0x04 f.push("WriteNoResp") end
        if flags & 0x08 f.push("Write") end
        if flags & 0x10 f.push("Notify") end
        if flags & 0x20 f.push("Indicate") end
        if flags & 0x40 f.push("AuthSignWrite") end
        if flags & 0x80 f.push("Extended") end
        return f
    end

    # BLE.buffer holds BLE.len payload bytes: the count, then the entries.
    def parseServices()
        var buf = BLE.buffer
        var i = 1
        self.services = []
        while i < BLE.len
            var sz = buf[i]/8                       # 16-, 32- or 128-bit -> 2, 4 or 16 bytes
            var uuid = buf[(i+1)..(i+sz)].reverse() # depending on line above
            i += 1+sz
            if sz == 16
                self.services.push(self.uuid128ToString(uuid))
            else
                self.services.push(uuid.tohex())
            end
        end
        self.getCharacteristics()
    end

    def getCharacteristics()
        print("__________________________")
        print("Service: UUID:",self.services[0])
        print("    Characteristics:")
        BLE.set_svc(self.services[0])
        BLE.run(7)
    end

    def parseCharacteristics()
        var buf = BLE.buffer
        var i = 1
        while i < BLE.len
            var sz = buf[i]/8
            var uuid = buf[(i+1)..(i+sz)].reverse()
            var flags = buf[i+sz+1]
            var handle = buf.get(i+sz+2,2)
            if sz == 16
                uuid = self.uuid128ToString(uuid)
            else
                uuid = uuid.tohex()
            end
            print("         UUID:",uuid,f", handle: 0x{handle:04x} ,",self.parseFlags(flags))
            i += 4+sz
        end
        if size(self.services) > 1
            self.services.remove(0)
            self.getCharacteristics()
        else
            print("_______________________________________________________________")
            print("Got all services and characteristics of connected BLE device!")
            print("###############################################################")
            BLE.run(5)
            print("BLE: disconnect device")
        end
    end

    def cb(error,op,uuid,handle)
        if error == 0
            if op == 5
                print("BLE: did disconnect")
            elif op == 6
                self.parseServices()
            elif op == 7
                self.parseCharacteristics();
            end
            return
        else
            print(error)
            if op == 6
                print("Try to connect to BLE device ...")
                BLE.run(6)
            end
        end
    end

end

ble_info = BLE_INFO()

# Usage:
# Berry console: ble_info.get("aabbccddeeff",type) # BLE device MAC and address type
