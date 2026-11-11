import Foundation

protocol Shape { var area: Double { get } }
struct Square: Shape, Codable, Equatable { var side: Double; var area: Double { side * side } }
final class Circle: Shape { let r: Double; init(r: Double) { self.r = r }; var area: Double { 3 * r * r } }
@objc class Speaker: NSObject { @objc func speak() -> String { "spoken through a selector" } }
enum Failure: Error { case expected(Int) }

func thrower(_ n: Int) throws -> Int { if n > 2 { throw Failure.expected(n) }; return n }

func describe(_ any: Any) -> String {
    if let s = any as? Shape { return "shape \(s.area)" }   // a conformance lookup at runtime
    return "not a shape"
}

let shapes: [Shape] = [Square(side: 2), Circle(r: 1)]
let total = shapes.map(\.area).reduce(0, +)
let data = try! JSONEncoder().encode(Square(side: 3))
let back = try! JSONDecoder().decode(Square.self, from: data)
var caught = "none"
do { _ = try thrower(5) } catch Failure.expected(let n) { caught = "caught \(n)" }
let spoken = Speaker().perform(NSSelectorFromString("speak"))?.takeUnretainedValue() as? String ?? "FAILED"
let typeName = String(describing: type(of: back))
let fromString = _typeByName("6swifty6SquareV") != nil ? "found" : "missing"
let sem = DispatchSemaphore(value: 0)
var asyncResult = 0
Task { asyncResult = await withTaskGroup(of: Int.self) { g in for i in 1...4 { g.addTask { i * i } }; return await g.reduce(0, +) }; sem.signal() }
sem.wait()
let dict = Dictionary(grouping: ["apple", "avocado", "banana"], by: { $0.first! })
print("total \(total) | json \(String(data: data, encoding: .utf8)!) back \(back == Square(side: 3)) | \(describe(Square(side: 1))) | \(describe(5)) | \(caught) | \(spoken) | \(typeName) \(fromString) | async \(asyncResult) | groups \(dict.keys.sorted())")
print("PASS swifty")
