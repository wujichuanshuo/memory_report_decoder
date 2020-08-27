#pragma once

#include "umpcrawler.h"

//#include <QTime>
//#include <QDebug>
#include <iostream>
#include <string>
#include <time.h>
#include <stdio.h>
#include <codecvt>
#include <locale>

#include <fstream>
static bool first = true;

#if _MSC_VER >= 1900

std::string utf16_to_utf8(std::u16string utf16_string)
{
	std::wstring_convert<std::codecvt_utf8_utf16<int16_t>, int16_t> convert;
	auto p = reinterpret_cast<const int16_t *>(utf16_string.data());
	return convert.to_bytes(p, p + utf16_string.size());
}
std::u16string utf8_to_utf16(std::string utf8_string)
{
	std::wstring_convert<std::codecvt_utf8_utf16<int16_t>, int16_t> convert;
	auto p = reinterpret_cast<const char *>(utf8_string.data());
	auto str = convert.from_bytes(p, p + utf8_string.size());
	std::u16string u16_str(str.begin(), str.end());
	return u16_str;
}
#else

std::string utf16_to_utf8(std::u16string utf16_string)
{
	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
	return convert.to_bytes(utf16_string);
}
std::u16string utf8_to_utf16(std::string utf8_string)
{
	std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
	return convert.from_bytes(utf8_string);
}
#endif

BytesAndOffset FindInHeap(Il2CppManagedMemorySnapshot* snapshot, std::uint64_t addr) {
	if (first) {
		for (std::uint32_t i = 0; i < snapshot->heap.sectionCount; i++) {
			auto section = snapshot->heap.sections[i];
			printf("heap section: index=%d, startAddr=%x, endAddr=%x, size=%d",
				i, section.sectionStartAddress, section.sectionStartAddress + static_cast<std::uint64_t>(section.sectionSize),
				static_cast<std::uint64_t>(section.sectionSize));
		}
		first = false;
	}

    BytesAndOffset ba;
    for (std::uint32_t i = 0; i < snapshot->heap.sectionCount; i++) {
        auto section = snapshot->heap.sections[i];
        if (addr >= section.sectionStartAddress && addr < (section.sectionStartAddress + static_cast<std::uint64_t>(section.sectionSize))) {
            ba.bytes_ = section.sectionBytes;
            ba.offset_ = addr - section.sectionStartAddress;
            ba.pointerSize_ = snapshot->runtimeInformation.pointerSize;
            break;
        }
    }
    return ba;
}

int ReadArrayLength(Il2CppManagedMemorySnapshot* snapshot, std::uint64_t address, Il2CppMetadataType* arrayType) {
    auto bo = FindInHeap(snapshot, address);
    auto bounds = bo.Add(snapshot->runtimeInformation.arrayBoundsOffsetInHeader).ReadPointer();
    if (bounds == 0)
        return bo.Add(snapshot->runtimeInformation.arraySizeOffsetInHeader).ReadInt32();
    auto cursor = FindInHeap(snapshot, bounds);
    int length = 1;
    int arrayRank = static_cast<int>(arrayType->flags & Il2CppMetadataTypeFlags::kArrayRankMask) >> 16;
    for (int i = 0; i < arrayRank; i++) {
        length *= cursor.ReadInt32();
        cursor = cursor.Add(8);
    }
    return length;
}

int ReadArrayObjectSizeInBytes(Il2CppManagedMemorySnapshot* snapshot, std::uint64_t address, Il2CppMetadataType* arrayType,
                               const std::vector<Il2CppMetadataType*>& typeDescriptions) {
    auto arrayLength = ReadArrayLength(snapshot, address, arrayType);
    auto elementType = typeDescriptions[arrayType->baseOrElementTypeIndex];
    auto elementSize = ((elementType->flags & Il2CppMetadataTypeFlags::kValueType) != 0) ? elementType->size : snapshot->runtimeInformation.pointerSize;
    return static_cast<int>(snapshot->runtimeInformation.arrayHeaderSize + elementSize * static_cast<unsigned int>(arrayLength));
}

int ReadStringObjectSizeInBytes(BytesAndOffset& bo, Il2CppManagedMemorySnapshot* snapshot) {
    auto lengthPointer = bo.Add(snapshot->runtimeInformation.objectHeaderSize);
    auto length = lengthPointer.ReadInt32();
    return static_cast<std::int32_t>(snapshot->runtimeInformation.objectHeaderSize) + 1 + (length + 2) + 2;
}

void Crawler::Crawl(PackedCrawlerData& result, Il2CppManagedMemorySnapshot* snapshot) {
	first = true;

    std::vector<PackedManagedObject> managedObjects;
    std::vector<Connection> connections;
    for (std::uint32_t i = 0; i < snapshot->metadata.typeCount; i++) {
        auto type = &snapshot->metadata.types[i];
        type->typeIndex = i;
        typeInfoToTypeDescription_.emplace(type->typeInfoAddress, type);
        typeDescriptions_.push_back(type);
    }
    // crawl pointers
	qDebug("--------------------- gcHandler ----------------");
    for (std::uint32_t i = 0; i < snapshot->gcHandles.trackedObjectCount; i++) {
        auto gcHandle = snapshot->gcHandles.pointersToObjects[i];
		qDebug("gcHandle %p", gcHandle);
        CrawlPointer(snapshot, result.startIndices_, gcHandle, result.startIndices_.OfFirstGCHandle() + i, connections, managedObjects, (void*)gcHandle, false);
    }
	qDebug("--------------------- /gcHandler ----------------");
    // crawl raw object data
	qDebug("--------------------- static ----------------");
    for (std::size_t i = 0; i < result.typesWithStaticFields_.size(); i++) {
        auto typeDescription = result.typesWithStaticFields_[i];
        BytesAndOffset ba;
        ba.bytes_ = typeDescription->statics;
        ba.offset_ = 0;
        ba.pointerSize_ = snapshot->runtimeInformation.pointerSize;
        CrawlRawObjectData(snapshot, result.startIndices_, ba, typeDescription,
                           true, result.startIndices_.OfFirstStaticFields() + static_cast<std::uint32_t>(i), connections, managedObjects, false);
    }
	qDebug("--------------------- /static ----------------");
    result.managedObjects_ = std::move(managedObjects);
    result.connections_ = std::move(connections);
    result.typeDescriptions_ = std::move(typeDescriptions_);
}

void Crawler::CrawlPointer(Il2CppManagedMemorySnapshot* snapshot, StartIndices startIndices, std::uint64_t pointer, std::uint32_t indexOfFrom,
                           std::vector<Connection>& outConnections, std::vector<PackedManagedObject>& outManagedObjects, void* refPtr, bool scanMemory) {
    auto bo = FindInHeap(snapshot, pointer);
    if (!bo.IsValid())
        return;
    std::uint64_t typeInfoAddress;
    std::uint32_t indexOfObject;
    bool wasAlreadyCrawled;

    ParseObjectHeader(startIndices, snapshot, pointer, typeInfoAddress, indexOfObject, wasAlreadyCrawled, outManagedObjects, refPtr, scanMemory);
    outConnections.push_back(Connection(indexOfFrom, indexOfObject));

    if (wasAlreadyCrawled)
        return;

    auto typeDescription = typeInfoToTypeDescription_[typeInfoAddress];
	qDebug("CrawPointer name=%s, assemblyName=%s, isArray=%d", typeDescription->name, typeDescription->assemblyName, (typeDescription->flags & Il2CppMetadataTypeFlags::kArray) != 0);
    if ((typeDescription->flags & Il2CppMetadataTypeFlags::kArray) == 0) {
        auto bo2 = bo.Add(snapshot->runtimeInformation.objectHeaderSize);
        CrawlRawObjectData(snapshot, startIndices, bo2, typeDescription, false, indexOfObject, outConnections, outManagedObjects, false);
        return;
    }
    auto arrayLen = ReadArrayLength(snapshot, pointer, typeDescription);
    auto elementType = typeDescriptions_[typeDescription->baseOrElementTypeIndex];
    auto cursor = bo.Add(snapshot->runtimeInformation.arrayHeaderSize);
    for (int i = 0; i != arrayLen; i++) {
        if ((elementType->flags & Il2CppMetadataTypeFlags::kValueType) != 0) {
            CrawlRawObjectData(snapshot, startIndices, cursor, elementType, false, indexOfObject, outConnections, outManagedObjects, true);
            cursor = cursor.Add(elementType->size);
        } else {
            CrawlPointer(snapshot, startIndices, cursor.ReadPointer(), indexOfObject, outConnections, outManagedObjects, refPtr, false);
            cursor = cursor.NextPointer();
        }
    }
}


void Crawler::FindObjectInHeap(Il2CppManagedMemorySnapshot* snapshot, std::uint64_t originalHeapAddress)
{
	auto bo = FindInHeap(snapshot, originalHeapAddress);
	if (bo.IsValid()) {
		auto pointer1 = bo.ReadPointer();
		if ((pointer1 & 1) == 0) {
			auto typeDescription = typeInfoToTypeDescription_[pointer1];
			auto &type = snapshot->metadata.types[typeDescription->typeIndex];
			qDebug("FindObjectInHeap MgrObj %p name=%s, assemblyName=%s, refPtr=%p", originalHeapAddress, type.name, type.assemblyName);
		}
		else
		{
			auto pointer = pointer1 & ~static_cast<std::uint64_t>(1);
			auto typeDescription = typeInfoToTypeDescription_[pointer];
			auto &type = snapshot->metadata.types[typeDescription->typeIndex];
			qDebug("FindObjectInHeap MgrObj %p name=%s, assemblyName=%s, refPtr=%p", originalHeapAddress, type.name, type.assemblyName);
		}
	}
	else {
		qDebug("can not found ptr %p in heap", originalHeapAddress);
	}
}


void Crawler::ParseObjectHeader(StartIndices& startIndices, Il2CppManagedMemorySnapshot* snapshot, std::uint64_t originalHeapAddress, std::uint64_t& typeInfoAddress,
                                std::uint32_t& indexOfObject, bool& wasAlreadyCrawled, std::vector<PackedManagedObject>& outManagedObjects, void* refPtr, bool scanMemory) {
    auto bo = FindInHeap(snapshot, originalHeapAddress);
    auto pointer1 = bo.ReadPointer();
    auto pointer2 = bo.NextPointer();
    if ((pointer1 & 1) == 0) {
        wasAlreadyCrawled = false;
        indexOfObject = static_cast<std::uint32_t>(outManagedObjects.size() + startIndices.OfFirstManagedObject());
        typeInfoAddress = pointer1;
        auto typeDescription = typeInfoToTypeDescription_[pointer1];
        auto size = SizeOfObjectInBytes(typeDescription, bo, snapshot, originalHeapAddress);
        PackedManagedObject managedObj;
        managedObj.address_ = originalHeapAddress;
        managedObj.size_ = static_cast<std::uint32_t>(size);
        managedObj.typeIndex_ = typeDescription->typeIndex;

		auto &type = snapshot->metadata.types[typeDescription->typeIndex];
		qDebug("add MgrObj %p name=%s, assemblyName=%s, refPtr=%p", originalHeapAddress, type.name, type.assemblyName, refPtr);

		uint32_t pointerSize = snapshot->runtimeInformation.pointerSize;
		if (managedObj.size_ % pointerSize == 0) 
		{
			BytesAndOffset startAddr = bo.Add(snapshot->runtimeInformation.objectHeaderSize);
			BytesAndOffset endAddr = startAddr.Add(managedObj.size_);
			qDebug("Start scan memory for obj, start addr=%p, size=%d]", startAddr.Cur(), managedObj.size_);

			auto ptr = 0;
			int count = 0;
			while (count < managedObj.size_) {
				ptr = startAddr.ReadPointer();
				FindObjectInHeap(snapshot, ptr);

				startAddr = startAddr.NextPointer();
				count += pointerSize;
			}
			qDebug("End scan memory for obj, end addr=%p, size=%d]", startAddr.Cur(), managedObj.size_);

		}
		else 
		{
			qDebug("bad obj size: %d", managedObj.size_);
		}

	
        outManagedObjects.push_back(managedObj);
        bo.WritePointer(pointer1 | 1);
        pointer2.WritePointer(static_cast<std::uint64_t>(indexOfObject));
        return;
    }
    typeInfoAddress = pointer1 & ~static_cast<std::uint64_t>(1);
    wasAlreadyCrawled = true;
    indexOfObject = static_cast<std::uint32_t>(pointer2.ReadPointer());
    return;
}

void AllFieldsOf(Il2CppMetadataType* typeDescription, std::vector<Il2CppMetadataType*>& typeDescriptions,
                 FieldFindOptions options, std::vector<Il2CppMetadataField*>& outFields) {
    std::vector<Il2CppMetadataType*> targetTypes = { typeDescription };
    while (!targetTypes.empty()) {
        auto curType = targetTypes.back();
        targetTypes.pop_back();
        if ((curType->flags & Il2CppMetadataTypeFlags::kArray) != 0)
            continue;
        // baseOrElementTypeIndex is Uint in unity source-code
        if (options != FieldFindOptions::OnlyStatic && curType->baseOrElementTypeIndex != static_cast<std::uint32_t>(-1)) {
            auto baseTypeDescription = typeDescriptions[curType->baseOrElementTypeIndex];
            targetTypes.push_back(baseTypeDescription);
        }
        for (std::uint32_t i = 0; i < curType->fieldCount; i++) {
            auto field = &curType->fields[i];
            if ((field->isStatic && options == FieldFindOptions::OnlyStatic) || (!field->isStatic && options == FieldFindOptions::OnlyInstance))
                outFields.push_back(field);
        }
    }
}

void Crawler::CrawlRawObjectData(Il2CppManagedMemorySnapshot* snapshot, StartIndices startIndices, BytesAndOffset bytesAndOffset,
                        Il2CppMetadataType* typeDescription, bool useStaticFields, std::uint32_t indexOfFrom,
                        std::vector<Connection>& outConnections, std::vector<PackedManagedObject>& outManagedObjects, bool isArrayElement) {
	qDebug("CrawlRawObjectData name=%s, assemblyName=%s", typeDescription->name, typeDescription->assemblyName);

    std::vector<Il2CppMetadataField*> fields;
    AllFieldsOf(typeDescription, typeDescriptions_, useStaticFields ? FieldFindOptions::OnlyStatic : FieldFindOptions::OnlyInstance, fields);
	int i = -1;
    for (auto& field : fields) {
		i++;
        if (field->typeIndex == typeDescription->typeIndex && (typeDescription->flags & Il2CppMetadataTypeFlags::kValueType) != 0)
            continue;
        // field.offset is Uint in unity source-code
        if (field->offset == static_cast<std::uint32_t>(-1))
            continue;
        auto fieldType = typeDescriptions_[field->typeIndex];
        auto fieldLocation = bytesAndOffset.Add(field->offset - (useStaticFields ? 0 : snapshot->runtimeInformation.objectHeaderSize));
		qDebug("%s.%s field index=%d name=%s", typeDescription->assemblyName, typeDescription->name, i, fieldType->name);
        if ((fieldType->flags & Il2CppMetadataTypeFlags::kValueType) != 0) {
            CrawlRawObjectData(snapshot, startIndices, fieldLocation, fieldType, false, indexOfFrom, outConnections, outManagedObjects, false);
            continue;
        }
		if (isArrayElement) { // elementClass is valueType(maybe a struct), but has refType filed
			qDebug("pointer in array");
		}
        // temporary workaround for a bug in 5.3b4 and earlier where we would get literals returned as fields with offset 0. soon we'll be able to remove this code.
        if (fieldLocation.pointerSize_ == 4 || fieldLocation.pointerSize_ == 8) {
            CrawlPointer(snapshot, startIndices, fieldLocation.ReadPointer(), indexOfFrom, outConnections, outManagedObjects, NULL, isArrayElement);
        }
    }
}

int Crawler::SizeOfObjectInBytes(Il2CppMetadataType* typeDescription, BytesAndOffset bo, Il2CppManagedMemorySnapshot* snapshot, std::uint64_t address) {
    if ((typeDescription->flags & Il2CppMetadataTypeFlags::kArray) != 0) {
        return ReadArrayObjectSizeInBytes(snapshot, address, typeDescription, typeDescriptions_);
    }
    if (std::string(typeDescription->name) == "System.String") {
        return ReadStringObjectSizeInBytes(bo, snapshot);
    }
    return static_cast<int>(typeDescription->size);
}

void CrawledMemorySnapshot::Unpack(CrawledMemorySnapshot& result, Il2CppManagedMemorySnapshot* snapshot, PackedCrawlerData& packedCrawlerData) {
    result.runtimeInformation_ = snapshot->runtimeInformation;
    // managed heap
    result.managedHeap_.resize(snapshot->heap.sectionCount);
    for (std::size_t i = 0; i < snapshot->heap.sectionCount; i++) {
        auto section = &snapshot->heap.sections[i];
        auto newSection = &result.managedHeap_[i];
        newSection->sectionSize_ = section->sectionSize;
        newSection->sectionStartAddress_ = section->sectionStartAddress;
        newSection->sectionBytes_ = new std::uint8_t[section->sectionSize];
        memcpy(newSection->sectionBytes_, section->sectionBytes, section->sectionSize);
    }
    // convert typeDescriptions
    result.typeDescriptions_.resize(packedCrawlerData.typeDescriptions_.size());
    for (std::size_t i = 0; i < packedCrawlerData.typeDescriptions_.size(); i++) {
        auto& from = packedCrawlerData.typeDescriptions_[i];
        auto& to = result.typeDescriptions_[i];
        to.flags_ = from->flags;
        if ((to.flags_ & Il2CppMetadataTypeFlags::kArray) == 0) {
            to.fields_.resize(from->fieldCount);
            for (std::uint32_t j = 0; j < from->fieldCount; j++) {
                auto& fromField = from->fields[j];
                auto& toField = to.fields_[j];
                toField.name_ = std::string(fromField.name);
                toField.offset_ = fromField.offset;
                toField.isStatic_ = fromField.isStatic;
                toField.typeIndex_ = fromField.typeIndex;
            }
            to.statics_ = new std::uint8_t[from->staticsSize];
            to.staticsSize_ = from->staticsSize;
            memcpy(to.statics_, from->statics, from->staticsSize);
        }
        to.baseOrElementTypeIndex_ = from->baseOrElementTypeIndex;
        to.name_ = std::string(from->name);
        to.assemblyName_ = std::string(from->assemblyName);
        to.typeInfoAddress_ = from->typeInfoAddress;
        to.size_ = from->size;
        to.typeIndex_ = from->typeIndex;
    }
    // unpack gchandle
    for (std::uint32_t i = 0; i < snapshot->gcHandles.trackedObjectCount; i++) {
        GCHandle handle;
        handle.size_ = snapshot->runtimeInformation.pointerSize;
        handle.caption_ = "gchandle";
        result.gcHandles_.push_back(handle);
    }
    // unpack statics
    for (auto type : packedCrawlerData.typesWithStaticFields_) {
        StaticFields field;
        field.typeDescription_ = &result.typeDescriptions_[type->typeIndex];
        field.caption_ = std::string("static field of ") + type->name;
        field.size_ = type->staticsSize;
        result.staticFields_.push_back(field);
    }
    // unpack managed
    for (auto& managed : packedCrawlerData.managedObjects_) {
        ManagedObject mo;
        mo.address_ = managed.address_;
        mo.size_ = managed.size_;
        mo.typeDescription_ = &result.typeDescriptions_[managed.typeIndex_];
        mo.caption_ = mo.typeDescription_->name_;
        result.managedObjects_.push_back(mo);
    }
    // combine
    result.allObjects_.reserve(result.gcHandles_.size() + result.staticFields_.size() + result.managedObjects_.size());
    std::uint32_t index = 0;
    for (auto& obj : result.gcHandles_) {
        obj.index_ = index++;
        result.allObjects_.push_back(&obj);
    }
    for (auto& obj : result.staticFields_) {
        obj.index_ = index++;
		std::hash<std::string> hash_str;
        obj.nameHash_ = hash_str(obj.typeDescription_->assemblyName_ + obj.caption_);
        result.allObjects_.push_back(&obj);
    }
    for (auto& obj : result.managedObjects_) {
        obj.index_ = index++;
        result.allObjects_.push_back(&obj);
    }
    // connections
    std::vector<std::vector<ThingInMemory*>> referencesLists(result.allObjects_.size());
    std::vector<std::vector<ThingInMemory*>> referencedByLists(result.allObjects_.size());
    for (auto& connection : packedCrawlerData.connections_) {
        referencesLists[connection.from_].push_back(result.allObjects_[connection.to_]);
        referencedByLists[connection.to_].push_back(result.allObjects_[connection.from_]);
    }
    for (std::size_t i = 0; i != result.allObjects_.size(); i++) {
        result.allObjects_[i]->references_ = std::move(referencesLists[i]);
        result.allObjects_[i]->referencedBy_ = std::move(referencedByLists[i]);
    }
}

BytesAndOffset CrawledMemorySnapshot::FindInHeap(const CrawledMemorySnapshot* snapshot, std::uint64_t addr) {
    BytesAndOffset ba;
    for (std::size_t i = 0; i < snapshot->managedHeap_.size(); i++) {
        auto section = snapshot->managedHeap_[i];
        if (addr >= section.sectionStartAddress_ && addr < (section.sectionStartAddress_ + static_cast<std::uint64_t>(section.sectionSize_))) {
            ba.bytes_ = section.sectionBytes_;
            ba.offset_ = addr - section.sectionStartAddress_;
            ba.pointerSize_ = snapshot->runtimeInformation_.pointerSize;
            break;
        }
    }
    return ba;
}

std::string CrawledMemorySnapshot::ReadString(const CrawledMemorySnapshot* snapshot, const BytesAndOffset& bo) {
    if (!bo.IsValid())
        return std::string();
    auto lengthPointer = bo.Add(snapshot->runtimeInformation_.objectHeaderSize);
    auto length = lengthPointer.ReadInt32();
    auto firstChar = lengthPointer.Add(4);
	std::string dest;
	std::u16string source;
	source = std::u16string(reinterpret_cast<char16_t*>(firstChar.bytes_ + firstChar.offset_),length);
	dest += utf16_to_utf8(source);
	return dest;
}

int CrawledMemorySnapshot::ReadArrayLength(const CrawledMemorySnapshot* snapshot, std::uint64_t address, TypeDescription* arrayType) {
    auto bo = FindInHeap(snapshot, address);
    auto bounds = bo.Add(snapshot->runtimeInformation_.arrayBoundsOffsetInHeader).ReadPointer();
    if (bounds == 0)
        return bo.Add(snapshot->runtimeInformation_.arraySizeOffsetInHeader).ReadInt32();
    auto cursor = FindInHeap(snapshot, bounds);
    int length = 1;
    int arrayRank = static_cast<int>(arrayType->flags_ & Il2CppMetadataTypeFlags::kArrayRankMask) >> 16;
    for (int i = 0; i < arrayRank; i++) {
        length *= cursor.ReadInt32();
        cursor = cursor.Add(8);
    }
    return length;
}

void CrawledMemorySnapshot::AllFieldsOf(const CrawledMemorySnapshot* snapshot, const TypeDescription* typeDescription,
                                        FieldFindOptions options, std::vector<const FieldDescription*>& outFields) {
    std::vector<const TypeDescription*> targetTypes = { typeDescription };
    while (!targetTypes.empty()) {
        auto curType = targetTypes.back();
        targetTypes.pop_back();
        if ((curType->flags_ & Il2CppMetadataTypeFlags::kArray) != 0)
            continue;
        // baseOrElementTypeIndex is Uint in unity source-code
        if (options != FieldFindOptions::OnlyStatic && curType->baseOrElementTypeIndex_ != static_cast<std::uint32_t>(-1)) {
            auto baseTypeDescription = &snapshot->typeDescriptions_[curType->baseOrElementTypeIndex_];
            targetTypes.push_back(baseTypeDescription);
        }
        for (std::size_t i = 0; i < curType->fields_.size(); i++) {
            auto field = &curType->fields_[i];
            if ((field->isStatic_ && options == FieldFindOptions::OnlyStatic) || (!field->isStatic_ && options == FieldFindOptions::OnlyInstance))
                outFields.push_back(field);
        }
    }
}

CrawledMemorySnapshot* CrawledMemorySnapshot::Clone(const CrawledMemorySnapshot* src) {
    auto clone = new CrawledMemorySnapshot();
    clone->runtimeInformation_ = src->runtimeInformation_;
    // managed heap
    clone->managedHeap_.resize(src->managedHeap_.size());
    for (std::size_t i = 0; i < src->managedHeap_.size(); i++) {
        auto section = &src->managedHeap_[i];
        auto newSection = &clone->managedHeap_[i];
        newSection->sectionSize_ = section->sectionSize_;
        newSection->sectionStartAddress_ = section->sectionStartAddress_;
        newSection->sectionBytes_ = new std::uint8_t[section->sectionSize_];
        memcpy(newSection->sectionBytes_, section->sectionBytes_, section->sectionSize_);
    }
    // typeDescriptions
    clone->typeDescriptions_.reserve(src->typeDescriptions_.size());
    for (auto& type : src->typeDescriptions_)
        clone->typeDescriptions_.push_back(TypeDescription(type));
    // gchandle
    clone->gcHandles_.reserve(src->gcHandles_.size());
    for (auto& gchandle : src->gcHandles_) {
        clone->gcHandles_.push_back(GCHandle(gchandle));
    }
    // statics
    clone->staticFields_.reserve(src->staticFields_.size());
    for (auto& staticFields : src->staticFields_) {
        clone->staticFields_.push_back(StaticFields(staticFields));
        auto& newStaticFields = clone->staticFields_.back();
        newStaticFields.typeDescription_ = &clone->typeDescriptions_[staticFields.typeDescription_->typeIndex_];
        newStaticFields.nameHash_ = staticFields.nameHash_;
    }
    clone->managedObjects_.reserve(src->managedObjects_.size());
    for (auto& managed : src->managedObjects_) {
        clone->managedObjects_.push_back(ManagedObject(managed));
        auto& newManaged = clone->managedObjects_.back();
        newManaged.address_ = managed.address_;
        newManaged.typeDescription_ = &clone->typeDescriptions_[managed.typeDescription_->typeIndex_];
    }
    // combine
    clone->allObjects_.reserve(src->allObjects_.size());
    std::uint32_t index = 0;
    for (auto& obj : clone->gcHandles_) {
        obj.index_ = index++;
        clone->allObjects_.push_back(&obj);
    }
    for (auto& obj : clone->staticFields_) {
        obj.index_ = index++;
        clone->allObjects_.push_back(&obj);
    }
    for (auto& obj : clone->managedObjects_) {
        obj.index_ = index++;
        clone->allObjects_.push_back(&obj);
    }
    // connections
    for (std::size_t i = 0; i < clone->allObjects_.size(); i++) {
        auto cloneObj = clone->allObjects_[i];
        auto secondObj = src->allObjects_[i];
        cloneObj->references_.reserve(secondObj->references_.size());
        for (auto& ref : secondObj->references_)
            cloneObj->references_.push_back(clone->allObjects_[ref->index_]);
        cloneObj->referencedBy_.reserve(secondObj->referencedBy_.size());
        for (auto& ref : secondObj->referencedBy_)
            cloneObj->referencedBy_.push_back(clone->allObjects_[ref->index_]);
    }
    return clone;
}

CrawledMemorySnapshot* CrawledMemorySnapshot::Diff(const CrawledMemorySnapshot* firstSnapshot, const CrawledMemorySnapshot* secondSnapshot) {
    auto diffed = CrawledMemorySnapshot::Clone(secondSnapshot);
    // managed
    std::unordered_map<std::uint64_t, const ManagedObject*> firstManagedObjects;
    for (auto& managed : firstSnapshot->managedObjects_) {
        firstManagedObjects[managed.address_] = &managed;
    }
    for (auto& managed : diffed->managedObjects_) {
        auto it = firstManagedObjects.find(managed.address_);
        if (it != firstManagedObjects.end()) {
            auto firstManaged = it->second;
            managed.size_ -= firstManaged->size_;
            if (managed.size_ == 0)
                managed.diff_ = CrawledDiffFlags::kSame;
            else if (managed.size_ > 0)
                managed.diff_ = CrawledDiffFlags::kBigger;
            else
                managed.diff_ = CrawledDiffFlags::kSmaller;
        } else {
            managed.diff_ = CrawledDiffFlags::kAdded;
        }
    }
    // statics
    std::unordered_map<std::uint64_t, const StaticFields*> firstStaticFields;
    for (auto& statics : firstSnapshot->staticFields_) {
        firstStaticFields[statics.nameHash_] = &statics;
    }
    for (auto& statics : diffed->staticFields_) {
        auto it = firstStaticFields.find(statics.nameHash_);
        if (it != firstStaticFields.end()) {
            auto firstStatics = it->second;
            statics.size_ -= firstStatics->size_;
            if (statics.size_ == 0)
                statics.diff_ = CrawledDiffFlags::kSame;
            else if (statics.size_ > 0)
                statics.diff_ = CrawledDiffFlags::kBigger;
            else
                statics.diff_ = CrawledDiffFlags::kSmaller;
        } else {
            statics.diff_ = CrawledDiffFlags::kAdded;
        }
    }
	time_t tt = time(NULL);
	tm* t = localtime(&tt);
	printf("%d-%02d-%02d %02d:%02d:%02d\n",
		t->tm_year + 1900,
		t->tm_mon + 1,
		t->tm_mday,
		t->tm_hour,
		t->tm_min,
		t->tm_sec);
	std::string time = std::string(std::to_string(t->tm_hour)+ std::to_string(t->tm_min)+ std::to_string(t->tm_sec));
    diffed->name_ = "Diff_" + time;
    diffed->isDiff_ = true;
    return diffed;
}

void CrawledMemorySnapshot::Free(CrawledMemorySnapshot* snapshot) {
    for (auto& section : snapshot->managedHeap_) {
        if (section.sectionSize_ > 0)
            delete[] section.sectionBytes_;
    }
    for (auto& type : snapshot->typeDescriptions_) {
        if (type.staticsSize_ > 0)
            delete[] type.statics_;
    }
}


void Il2CppFreeMemorySnapshot(Il2CppManagedMemorySnapshot* snapshot) {
	if (snapshot->heap.sectionCount > 0) {
		for (uint32_t i = 0; i < snapshot->heap.sectionCount; i++) {
			delete[] snapshot->heap.sections[i].sectionBytes;
		}
		delete[] snapshot->heap.sections;
		snapshot->heap.sectionCount = 0;
		snapshot->heap.sections = nullptr;
	}
	if (snapshot->stacks.stackCount > 0) {
		for (uint32_t i = 0; i < snapshot->stacks.stackCount; i++) {
			delete[] snapshot->stacks.stacks[i].sectionBytes;
		}
		delete[] snapshot->stacks.stacks;
		snapshot->stacks.stackCount = 0;
		snapshot->stacks.stacks = nullptr;
	}
	if (snapshot->gcHandles.trackedObjectCount > 0) {
		delete[] snapshot->gcHandles.pointersToObjects;
		snapshot->gcHandles.pointersToObjects = nullptr;
		snapshot->gcHandles.trackedObjectCount = 0;
	}
	if (snapshot->metadata.typeCount > 0) {
		for (uint32_t i = 0; i < snapshot->metadata.typeCount; i++) {
			auto& type = snapshot->metadata.types[i];
			if ((type.flags & kArray) == 0) {
				for (uint32_t j = 0; j < type.fieldCount; j++) {
					auto& field = type.fields[j];
					delete[] field.name;
				}
				delete[] type.fields;
				delete[] type.statics;
			}
			delete[] type.name;
			delete[] type.assemblyName;
		}
		delete[] snapshot->metadata.types;
		snapshot->metadata.types = nullptr;
		snapshot->metadata.typeCount = 0;
	}
}

class RemoteProcess {
public:
	RemoteProcess();
	~RemoteProcess();

	void ConnectToServer(int port);
	void Disconnect();
	bool IsConnecting() const { return connectingServer_; }
	bool IsConnected() const { return serverConnected_; }

	const Il2CppManagedMemorySnapshot* GetSnapShot() const { return snapShot_; }
	Il2CppManagedMemorySnapshot* GetSnapShot() { return snapShot_; }
	void SetSnapShot(Il2CppManagedMemorySnapshot* snapShot) {
		snapShot_ = snapShot;
	}

	bool DecodeData(const char* data, size_t size, bool isBigEndian);


private:
	void OnDataReceived();
	void OnConnected();
	void OnDisconnected();

private:
	bool connectingServer_ = false;
	bool serverConnected_ = false;
	char* buffer_ = nullptr;
	char* compressBuffer_ = nullptr;
	Il2CppManagedMemorySnapshot *snapShot_ = nullptr;
};

bool RemoteProcess::DecodeData(const char* data, size_t size, bool isBigEndian) {
	Il2CppFreeMemorySnapshot(snapShot_);
	if (size < 8)
		return false;
	bufferreader reader(data, size, isBigEndian);
	std::uint32_t magic, version;
	reader >> magic >> version;
	if (magic != kSnapshotMagicBytes) {
		std::cout << "Invalide MagicBytes!" << magic << kSnapshotMagicBytes;
		return false;
	}
	if (version > kSnapshotFormatVersion) {
		std::cout << "Version Missmatch!";
		return false;
	}
	while (!reader.atEnd()) {
		reader >> magic;
		if (magic == kSnapshotHeapMagicBytes) {
			reader >> snapShot_->heap.sectionCount;
			snapShot_->heap.sections = new Il2CppManagedMemorySection[snapShot_->heap.sectionCount];
			for (std::uint32_t i = 0; i < snapShot_->heap.sectionCount; i++) {
				auto& section = snapShot_->heap.sections[i];
				reader >> section.sectionStartAddress >> section.sectionSize;
				section.sectionBytes = new std::uint8_t[section.sectionSize];
				reader.read(reinterpret_cast<char*>(section.sectionBytes), section.sectionSize);
			}
		}
		else if (magic == kSnapshotStacksMagicBytes) {
			reader >> snapShot_->stacks.stackCount;
			snapShot_->stacks.stacks = new Il2CppManagedMemorySection[snapShot_->stacks.stackCount];
			for (std::uint32_t i = 0; i < snapShot_->stacks.stackCount; i++) {
				auto& section = snapShot_->stacks.stacks[i];
				reader >> section.sectionStartAddress >> section.sectionSize;
				section.sectionBytes = new std::uint8_t[section.sectionSize];
				reader.read(reinterpret_cast<char*>(section.sectionBytes), section.sectionSize);
			}
		}
		else if (magic == kSnapshotMetadataMagicBytes) {
			reader >> snapShot_->metadata.typeCount;
			snapShot_->metadata.types = new Il2CppMetadataType[snapShot_->metadata.typeCount];
			for (std::uint32_t i = 0; i < snapShot_->metadata.typeCount; i++) {
				auto& type = snapShot_->metadata.types[i];
				std::uint32_t flags;
				reader >> flags >> type.baseOrElementTypeIndex;
				type.flags = static_cast<Il2CppMetadataTypeFlags>(flags);
				if ((type.flags & Il2CppMetadataTypeFlags::kArray) == 0) {
					reader >> type.fieldCount;
					type.fields = new Il2CppMetadataField[type.fieldCount];
					for (uint32_t j = 0; j < type.fieldCount; j++) {
						auto& field = type.fields[j];
						reader >> field.offset >> field.typeIndex >> field.name >> field.isStatic;
					}
					reader >> type.staticsSize;
					type.statics = new std::uint8_t[type.staticsSize];
					reader.read(reinterpret_cast<char*>(type.statics), type.staticsSize);
				}
				else {
					type.statics = nullptr;
					type.staticsSize = 0;
					type.fields = nullptr;
					type.fieldCount = 0;
				}
				reader >> type.name >> type.assemblyName >> type.typeInfoAddress >> type.size;
			}
		}
		else if (magic == kSnapshotGCHandlesMagicBytes) {
			reader >> snapShot_->gcHandles.trackedObjectCount;
			snapShot_->gcHandles.pointersToObjects = new std::uint64_t[snapShot_->gcHandles.trackedObjectCount];
			for (std::uint32_t i = 0; i < snapShot_->gcHandles.trackedObjectCount; i++) {
				reader >> snapShot_->gcHandles.pointersToObjects[i];
			}
		}
		else if (magic == kSnapshotRuntimeInfoMagicBytes) {
			reader >> snapShot_->runtimeInformation.pointerSize >> snapShot_->runtimeInformation.objectHeaderSize >>
				snapShot_->runtimeInformation.arrayHeaderSize >> snapShot_->runtimeInformation.arrayBoundsOffsetInHeader >>
				snapShot_->runtimeInformation.arraySizeOffsetInHeader >> snapShot_->runtimeInformation.allocationGranularity;
		}
		else if (magic == kSnapshotTailMagicBytes) {
			break;
		}
		else {
			std::cout << "Unknown Section!";
			return false;
		}
	}
	return true;
}



class Windows
{
public:
	int LoadFromFile(std::string filepath);
private:
	void Windows::CleanWorkSpace();
	void Windows::RemoteDataReceived();
	void Windows::ShowSnapshot(CrawledMemorySnapshot* crawled);
private:
	int remoteRetryCount_ = 0;
	RemoteProcess *remoteProcess_;

};



int Windows::LoadFromFile(std::string filepath) {
	bool isRawFile = 1;
	if (filepath.find(".rawsnapshot")<0) {
		std::cout << "This is fail path!";
		isRawFile = 0;
		return 0;
	}
	//std::cout << "True" << std::endl;
	std::ifstream f(filepath , std::ios::binary | std::ios::in);
	if (!f) {
		return 0;
	}
	if (isRawFile) {
		unsigned char a;
		std::string tmp = "";
		while (f.read((char *)&a , sizeof(a))) {
			tmp +=  a;
		}
		unsigned char* file = (unsigned char*)tmp.c_str();
		unsigned char* snapshot = file;
		remoteProcess_->DecodeData((char*)snapshot, tmp.size(), false);
		tmp.clear();
		f.close();
		RemoteDataReceived();
	}

	return 0;
}

void Windows::RemoteDataReceived() {
	remoteRetryCount_ = 5;

	Crawler crawler;
	Il2CppManagedMemorySnapshot* snapshot = remoteProcess_->GetSnapShot();
	auto packedCrawlerData = new PackedCrawlerData(snapshot);
	crawler.Crawl(*packedCrawlerData, snapshot);
	auto crawled = new CrawledMemorySnapshot();
	crawled->Unpack(*crawled, snapshot, *packedCrawlerData);
	delete packedCrawlerData;
	time_t tt = time(NULL);
	tm* t = localtime(&tt);
	printf("%d-%02d-%02d %02d:%02d:%02d\n",
		t->tm_year + 1900,
		t->tm_mon + 1,
		t->tm_mday,
		t->tm_hour,
		t->tm_min,
		t->tm_sec);
	std::string time = std::string(std::to_string(t->tm_hour) + std::to_string(t->tm_min) + std::to_string(t->tm_sec));
	crawled->name_ = "Snapshot_" + time;
	
	std::cout<<("Snapshot Received And Unpacked.");
}


