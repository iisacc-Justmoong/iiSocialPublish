# Optional cross-repository check. Run with Society's existing json_schemer
# environment; the SDK itself has no dependency on the Society application.
require "json"
require "json_schemer"

binary, contracts = ARGV
abort "usage: VerifySocietyContract.rb TEST_BINARY SOCIETY_CONTRACT_DIR" unless binary && contracts

documents = JSON.parse(IO.popen([binary, "--contract-fixtures"], &:read))
abort "fixture generation failed" unless $?.success?
schemas = Dir[File.join(contracts, "*.schema.json")].to_h do |file|
  schema = JSON.parse(File.read(file))
  [schema.fetch("$id"), schema]
end
schema = schemas.fetch("https://society.iisacc.com/contracts/v1/publishing-request.schema.json")
validator = JSONSchemer.schema(schema, ref_resolver: ->(uri) { schemas.fetch(uri.to_s) })
documents.each_with_index do |document, index|
  errors = validator.validate(document).to_a
  abort "Society request fixture #{index} failed: #{JSON.generate(errors)}" unless errors.empty?
end
puts "#{documents.length} Society publishing-request fixtures passed the current service schema"
