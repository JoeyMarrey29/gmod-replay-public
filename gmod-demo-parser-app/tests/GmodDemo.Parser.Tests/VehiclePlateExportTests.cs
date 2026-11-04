using GmodDemo.Cli;

namespace GmodDemo.Parser.Tests;

public class VehiclePlateExportTests
{
    [Fact]
    public void Original_placements_inheritance_bodykits_and_skins_are_preserved()
    {
        var result = PerpData.VehiclePlates([
            ("_unmapped/vehicles.lua", """
                local CIV_VEHICLE = {}
                CIV_VEHICLE.ID = "C"
                CIV_VEHICLE.LicensePlate = {Pos=Vector(0,-100,20),Ang=Angle(-5,270,0)}
                CIV_VEHICLE.CustomBodyGroup = {
                    {Groups={0}},
                    {Groups={1},LicensePlate={Pos=Vector(0,-105,22),Ang=Angle(0,270,0)}}
                }
                GM:RegisterVehicle(CIV_VEHICLE)
                local VEHICLE = table.shallow_copy(CIV_VEHICLE)
                VEHICLE.ID = "P"
                VEHICLE.LicensePlates = {CIV_VEHICLE.LicensePlate,
                    {Pos=Vector(0,100,21),Ang=Angle(0,90,0)}}
                GM:RegisterVehicle(VEHICLE)
                """),
            ("_unmapped/skins.lua", """
                ITEM.ID = "license_plate_skin"
                ITEM.BuyableItems = {{VariationID="blue",PlateTexture="models/plate_blue",
                                     TextColor=Color(226,228,231)}}
                """)
        ]);
        var vehicles = result["vehicles"]!.AsArray();
        Assert.Equal(2, vehicles.Count);
        Assert.Single(vehicles[0]!["placements"]!.AsArray());
        Assert.Equal(2, vehicles[1]!["placements"]!.AsArray().Count);
        Assert.Equal(-100, vehicles[1]!["placements"]![0]!["pos"]![1]!.GetValue<long>());
        Assert.Equal(100, vehicles[1]!["placements"]![1]!["pos"]![1]!.GetValue<long>());
        Assert.Equal(-105, vehicles[0]!["kits"]![1]!["plate"]!["pos"]![1]!.GetValue<long>());
        Assert.Equal("models/plate_blue", result["skins"]![0]!["material"]!.GetValue<string>());
        Assert.Equal(226, result["skins"]![0]!["color"]![0]!.GetValue<long>());
    }

    [Fact]
    public void Broken_vehicle_definitions_fail_explicitly()
    {
        Assert.Throws<InvalidDataException>(() => PerpData.VehiclePlates([
            ("broken.lua", "local VEHICLE = nil; GM:RegisterVehicle(VEHICLE)")
        ]));
    }

    [RealDemoFact]
    public void Recorded_server_exports_vehicle_plates_and_skin_materials()
    {
        var tree = PerpData.LoadLua(RealDemo.Path, null);
        var result = PerpData.VehiclePlates(tree.Files);
        Assert.True(result["vehicles"]!.AsArray().Count > 100);
        Assert.True(result["skins"]!.AsArray().Count > 10);
    }
}
