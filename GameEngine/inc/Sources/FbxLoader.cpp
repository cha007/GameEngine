#include "enginepch.h"

#include "FbxLoader.h"

namespace GameEngine
{
	namespace FbxLoader
	{
		using namespace std;

		Node::Node(const std::string& name, const std::string& parent)
		{
			this->name = name;
			parentName = parent;
		}

		Node::Node(Node&& node)
		{
			name = node.name;
			parentName = node.parentName;
			type = node.type;
			controlPoints.swap(node.controlPoints);
			materialNames.swap(node.materialNames);
			vertIndices.swap(node.vertIndices);
			vertexCountOfSubMesh.swap(node.vertexCountOfSubMesh);
			meshes.swap(node.meshes);
			childNodes.swap(node.childNodes);
			bones.swap(node.bones);
			matrix = node.matrix;
			useNormalMap = node.useNormalMap;
			useSkinnedMesh = node.useSkinnedMesh;
		}

		FbxLoader::~FbxLoader()
		{
			Release();
		}

		void FbxLoader::Release()
		{
			scene->Clear();
		}

		FbxLoader::FbxLoader()
		{
			
		}

		void FbxLoader::LoadFromFile(const std::string& folder, const std::string& name, AxisMode axismode, float scaleFactor)
		{
			FbxManager* fbxManager = FbxManager::Create();
			FbxIOSettings* ios = FbxIOSettings::Create(fbxManager, IOSROOT);
			fbxManager->SetIOSettings(ios);


			std::string path = folder + "\\" + name;
			relativeFolder = folder;
			FbxImporter* importer = (FbxImporter::Create(fbxManager, ""));
			bool b = importer->Initialize(path.c_str(), -1, fbxManager->GetIOSettings());
			if(!b) {
			}
			scene = FbxScene::Create(fbxManager, path.c_str());
			importer->Import(scene);
			

			int fileMajor, fileMinor, fileRevision;
			importer->GetFileVersion(fileMajor, fileMinor, fileRevision);
			importer->Destroy();

			factor = scaleFactor;
			this->axismode = axismode;
			FbxNode* root = scene->GetRootNode();
			
			FbxGeometryConverter geometryConverter(fbxManager);
						
			FbxAxisSystem axis = FbxAxisSystem::MayaYUp;	
			axis.ConvertScene(scene);
			
			geometryConverter.Triangulate(scene, false);

			ProcessMaterial(scene);

			LoadAnimationClipData();

			ProcessMeshNode(root, rootNode);			
		}

		void FbxLoader::ProcessMeshNode(FbxNode* node, Node& mnode)
		{
			if(!node)
				return;

			if(node->GetNodeAttribute()) {
				auto type = node->GetNodeAttribute()->GetAttributeType();
				if(type == FbxNodeAttribute::eMesh) {
					mnode.type = Node::eMesh;
					ProcessControlPoints(node, mnode);

					ProcessBoneAndAnimation(node, mnode);

					ProcessMesh(node, mnode);
					BindMaterial(node, mnode);
				}
				else if(type == FbxNodeAttribute::eSkeleton) {
					mnode.type = Node::eBone;
				}
			}

			ComputeNodeMatrix(node, mnode, true);
			const int count = node->GetChildCount();
			mnode.childNodes.resize(count);

			for(int i = 0; i < count; i++) {
				auto child = node->GetChild(i);
				auto& childNode = mnode.childNodes[i];
				childNode.name = child->GetName();
				childNode.parentName = mnode.name;
				ProcessMeshNode(node->GetChild(i), childNode);
			}
		}

		void FbxLoader::ProcessControlPoints(FbxNode* node, Node& meshNode)
		{
			FbxMesh* currMesh = node->GetMesh();
			if(!currMesh)
				return;

			const int ctrlPointCount = currMesh->GetControlPointsCount();
			meshNode.controlPoints.resize(ctrlPointCount);

			for(int i = 0; i < ctrlPointCount; i++) {
				FbxVector4 p = currMesh->GetControlPointAt(i);
				auto& cp = meshNode.controlPoints[i];
				cp.position = { (float)p[0], (float)p[1], (float)p[2] };
			}
		}

		void FbxLoader::ProcessMesh(FbxNode* node, Node& meshNode)
		{
			FbxMesh* currMesh = node->GetMesh();

			if(!currMesh)
				return;

			FbxLayerElementTangent* tangents = nullptr;

			if(useNormalMap) {
				if(currMesh->GetElementTangentCount() < 1) {
					currMesh->GenerateTangentsDataForAllUVSets();
					tangents = currMesh->GetElementTangent();
				}
			}

			std::map<int, std::vector<Vertex>> subMeshes;

			int vertCounter = 0;
			const int polygonCount = currMesh->GetPolygonCount();

			for(int i = 0; i < polygonCount; i++) {
				const int polySize = currMesh->GetPolygonSize(i);
				int nMaterials = node->GetMaterialCount();
				auto elementMaterial = currMesh->GetElementMaterial();
				int mi = 0;
				if(elementMaterial)
					mi = currMesh->GetElementMaterial()->GetIndexArray()[i];

				for(int j = 2; j >= 0; --j) {
					int ctrlIndex = currMesh->GetPolygonVertex(i, j);
					auto& currCtrlPoint = meshNode.controlPoints[ctrlIndex];
					FbxVector4 v4;

					auto& pos = currCtrlPoint.position * factor;
					currMesh->GetPolygonVertexNormal(i, j, v4);
					Vector3 normal = { (float)v4[0], (float)v4[1], (float)v4[2] };
					Vector3 tangent = { 0, 0, 0 };
					if(useNormalMap) {
						ReadTangent(tangents, ctrlIndex, vertCounter, v4);
						tangent = { (float)v4[0], (float)v4[1], (float)v4[2] };
					}
				
					Vector2 uv;
					FbxStringList uvSetNames;
					currMesh->GetUVSetNames(uvSetNames);
					bool unmapped = false;
					// supports one uv set only
					if(uvSetNames.GetCount() > 0) {
						FbxVector2 UV;
						currMesh->GetPolygonVertexUV(i, j, uvSetNames[0], UV, unmapped);
						uv = { (float)UV[0], (float)UV[1] };
					}
				
					if(axismode == eLeftHanded) {
						pos.x *= -1; uv.y = 1 - uv.y;
						normal.x *= -1; tangent.x *= -1;
					}

					Vector4 weights = { 0, 0, 0, 0 };
					Byte4 indices = { 0, 0, 0, 0 };

					int blendCount = (int)min(currCtrlPoint.blendWeigths.size(), 4);
					if(blendCount > 0) {
						meshNode.useSkinnedMesh = true;
						if(currCtrlPoint.blendWeigths.size() > 4)
							sort(currCtrlPoint.blendWeigths.begin(), currCtrlPoint.blendWeigths.end());
						for(int i = 0; i < blendCount; i++) {
							weights[i] = currCtrlPoint.blendWeigths[i].weight;
							indices.m[i] = currCtrlPoint.blendWeigths[i].boneIndex;
						}
					}
	
					Vertex temp = { pos, uv, normal, tangent, indices, weights };
					subMeshes[mi].push_back(temp);
				}
				++vertCounter;
			}

			if(subMeshes.size() > 0) {
				int index = 0;
				meshNode.meshes.reserve(vertCounter);
				meshNode.vertIndices.reserve(vertCounter);
				meshNode.vertexCountOfSubMesh.reserve(subMeshes.size());
				for(auto& pair : subMeshes) {
					auto& m = pair.second;
					for(int i = 0; i < m.size(); ++i)
						meshNode.vertIndices.emplace_back(index++);
					meshNode.vertexCountOfSubMesh.push_back((int)m.size());
					meshNode.meshes.insert(meshNode.meshes.end(), m.begin(), m.end());
				}
			}
			subMeshes.clear();
		}

		void FbxLoader::BindMaterial(FbxNode* node, Node& meshNode)
		{
			const int materialCount = node->GetMaterialCount();
			meshNode.materialNames.resize(materialCount);

			for(int i = 0; i < materialCount; i++) {
				FbxSurfaceMaterial* surfaceMaterial = node->GetMaterial(i);
				auto name = surfaceMaterial->GetName();
				meshNode.materialNames[i] = name;
				if(materials[name].normalMapName != "")
					meshNode.useNormalMap = true;
			}
		}

		void FbxLoader::ReadTangent(FbxLayerElementTangent* tangents, int ctrlIndex, int vertexCount, FbxVector4& vector)
		{
			auto mappingMode = tangents->GetMappingMode();
			auto refMode = tangents->GetReferenceMode();

			switch(mappingMode) {
				case FbxGeometryElement::eByControlPoint:
					switch(refMode) {
						case FbxGeometryElement::eDirect:
						{
							vector = tangents->GetDirectArray().GetAt(ctrlIndex);
							break;
						}
						case FbxGeometryElement::eIndexToDirect:
						{
							int index = tangents->GetIndexArray().GetAt(ctrlIndex);
							vector = tangents->GetDirectArray().GetAt(index);
							break;
						}
						default:
							break;
					}
				case FbxGeometryElement::eByPolygonVertex:
					switch(refMode) {
						case FbxGeometryElement::eDirect:
						{
							vector = tangents->GetDirectArray().GetAt(vertexCount);
							break;
						}
						case FbxGeometryElement::eIndexToDirect:
						{
							int index = tangents->GetIndexArray().GetAt(vertexCount);
							vector = tangents->GetDirectArray().GetAt(index);
							break;
						}
						default:
							break;
					}
			}
		}

		void FbxLoader::ProcessMaterial(FbxScene* scene)
		{
			const int materialCount = scene->GetMaterialCount();
			for(int i = 0; i < materialCount; i++) {
				FbxSurfaceMaterial* surfaceMaterial = scene->GetMaterial(i);
				FMaterial material;
				ProcessMaterialAttribute(surfaceMaterial, material);
				ProcessMaterialTexture2D(surfaceMaterial, material);
				materials.insert({ material.name, std::move(material) });
			}
		}

		void FbxLoader::ProcessMaterialAttribute(FbxSurfaceMaterial* mat, FMaterial& material)
		{
			//FbxDouble3 d3;
			//FbxDouble d1;

			material.type = FMaterial::MT_INVALID;
			material.name = mat->GetName();
			//material->mName = mat->GetName();

			//legacy material type
			//std::string className = mat->GetClassId().GetName();
			//if(className == "FbxSurfaceLambert") {
			//	material->type = FMaterial::MT_LAMBERT;

			//	d3 = reinterpret_cast<FbxSurfaceLambert*>(mat)->Ambient;
			//	material->mAmbient = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d3 = reinterpret_cast<FbxSurfaceLambert*>(mat)->Diffuse;
			//	material->mDiffuse = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d3 = reinterpret_cast<FbxSurfaceLambert*>(mat)->Emissive;
			//	material->mEmissive = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d1 = reinterpret_cast<FbxSurfaceLambert*>(mat)->TransparencyFactor;
			//	material->mTransperencyFactor = (float)d1;
			//}
			//// phong model
			//else if(className == "FbxSurfacePhong") {
			//	material->type = FMaterial::MT_PHONG;
			//	d3 = reinterpret_cast<FbxSurfacePhong*>(mat)->Ambient;
			//	material->mAmbient = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d3 = reinterpret_cast<FbxSurfacePhong*>(mat)->Diffuse;
			//	material->mDiffuse = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d3 = reinterpret_cast<FbxSurfacePhong*>(mat)->Emissive;
			//	material->mEmissive = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d1 = reinterpret_cast<FbxSurfacePhong*>(mat)->TransparencyFactor;
			//	material->mTransperencyFactor = (float)d1;

			//	d3 = reinterpret_cast<FbxSurfacePhong*>(mat)->Specular;
			//	material->mSpecular = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d3 = reinterpret_cast<FbxSurfacePhong*>(mat)->Reflection;
			//	material->mReflection = Color((float)d3[0], (float)d3[1], (float)d3[2], 1);

			//	d1 = reinterpret_cast<FbxSurfacePhong*>(mat)->Shininess;
			//	material->mShininess = (float)d1;

			//	d1 = reinterpret_cast<FbxSurfacePhong*>(mat)->ReflectionFactor;
			//	material->mReflectionFactor = (float)d1;

			//	d1 = reinterpret_cast<FbxSurfacePhong*>(mat)->SpecularFactor;
			//	material->mSpecularPower = (float)d1;
			//}
		}

		void FbxLoader::FbxLoader::ProcessMaterialTexture2D(FbxSurfaceMaterial* mat, FMaterial& material)
		{
			int Texture2DIndex = 0;
			FbxProperty prop;

			FBXSDK_FOR_EACH_TEXTURE(Texture2DIndex)
			{
				prop = mat->FindProperty(FbxLayerElement::sTextureChannelNames[Texture2DIndex]);
				if(prop.IsValid()) {
					const int Texture2DCount = prop.GetSrcObjectCount<FbxTexture>();
					for(int i = 0; i < Texture2DCount; i++) {
						FbxTexture* texture = prop.GetSrcObject<FbxTexture>(i);
						if(texture) {
							std::string textureType = prop.GetNameAsCStr();
							FbxFileTexture* fileTexture = FbxCast<FbxFileTexture>(texture);
							if(fileTexture) {
								if(textureType == "DiffuseColor")
									material.diffuseMapName = fileTexture->GetRelativeFileName();
								else if(textureType == "SpecularColor")
									material.specularMapName = fileTexture->GetRelativeFileName();
								else if(textureType == "NormalMap") {
									material.normalMapName = fileTexture->GetRelativeFileName();
									useNormalMap = true;
								}
							}
						}
					}

					const int layeredTextureCount = prop.GetSrcObjectCount<FbxLayeredTexture>();
					for(int i = 0; i < layeredTextureCount; i++) {
						auto layeredTexture = prop.GetSrcObject<FbxLayeredTexture>(i);
						const int fileCount = layeredTexture->GetSrcObjectCount<FbxFileTexture>();
						for(int j = 0; j < fileCount; j++) {
							auto fileTexture = layeredTexture->GetSrcObject<FbxFileTexture>(j);
							std::string Texture2DType = prop.GetNameAsCStr();
							if(fileTexture) {
								if(Texture2DType == "DiffuseColor")
									material.diffuseMapName = fileTexture->GetRelativeFileName();
								else if(Texture2DType == "SpecularColor")
									material.specularMapName = fileTexture->GetRelativeFileName();
								else if(Texture2DType == "NormalMap") {
									material.normalMapName = fileTexture->GetRelativeFileName();
									useNormalMap = true;
								}
							}
						}
					}
				}
			}

			if(material.diffuseMapName != "") {
				std::string n = FbxPathUtils::GetFileName(material.diffuseMapName.c_str(), false).Buffer();
				material.name = n;
				mat->SetName(n.c_str());
			}
		}

		void FbxLoader::ProcessBoneAndAnimation(FbxNode* node, Node& meshNode)
		{
			auto currMesh = node->GetMesh();
			if(!currMesh) return;

			FbxVector4 lT = node->GetGeometricTranslation(FbxNode::eSourcePivot);
			FbxVector4 lR = node->GetGeometricRotation(FbxNode::eSourcePivot);
			FbxVector4 lS = node->GetGeometricScaling(FbxNode::eSourcePivot);

			FbxAMatrix geometryTransform = FbxAMatrix(lT, lR, lS);

			FbxSkin* skin = nullptr;
			const int deformerCnt = currMesh->GetDeformerCount();
			for(int deformerIndex = 0; deformerIndex < deformerCnt; ++deformerIndex) {
				skin = (FbxSkin*)(currMesh->GetDeformer(deformerIndex, FbxDeformer::eSkin));
				if(skin) break;
			}
			if(!skin) return;

			meshNode.useSkinnedMesh = true;
			const size_t nClusters = skin->GetClusterCount();
			if(nClusters < 1) return;

			for(auto& clip : animationClips)
				clip.second->transformCurves.resize(nClusters);

			meshNode.bones.resize(nClusters);
			const int animCount = scene->GetSrcObjectCount<FbxAnimStack>();

			float time = 0;
			for(int ci = 0; ci < nClusters; ++ci) {
				FbxCluster* cluster = skin->GetCluster(ci);
				auto elink = cluster->GetLinkMode();
				std::string boneName = cluster->GetLink()->GetName();

				FbxAMatrix transformMatrix, transformLinkMatrix, globalBindposeInverse;
				cluster->GetTransformMatrix(transformMatrix);
				cluster->GetTransformLinkMatrix(transformLinkMatrix);
				globalBindposeInverse = transformLinkMatrix.Inverse() * geometryTransform * transformMatrix;

				FbxNode* boneNode = cluster->GetLink();
				Bone& bone = meshNode.bones[ci];
				bone.name = boneName;
				bone.index = ci;

				auto T = globalBindposeInverse.GetT() * factor;
				
				if(axismode == eLeftHanded) {
					auto R = globalBindposeInverse.GetR();
					T[0] *= -1; R[1] *= -1; R[2] *= -1;
					globalBindposeInverse.SetR(R);
				}
				globalBindposeInverse.SetT(T);

				ConvertMatrix(bone.bindPoseInverse, globalBindposeInverse);

				const int nCtrl = cluster->GetControlPointIndicesCount();

				for(int ctrlIndex = 0; ctrlIndex < nCtrl; ++ctrlIndex) {
					BlendWeightPair pair;
					pair.boneIndex = ci;
					pair.weight = (float)cluster->GetControlPointWeights()[ctrlIndex];
					meshNode.controlPoints[cluster->GetControlPointIndices()[ctrlIndex]].blendWeigths.push_back(pair);
				}

				FbxAMatrix preRot;
				auto preR = boneNode->GetPreRotation(FbxNode::eSourcePivot);
				preRot.SetR(preR);

				for(int ai = 0; ai < animCount; ++ai) {
					auto animStack = scene->GetSrcObject<FbxAnimStack>(ai);
					scene->SetCurrentAnimationStack(animStack);
					std::string animName = animStack->GetName();
					auto& clip = animationClips[animName];
					auto& transformCurve = clip->transformCurves[ci];
					transformCurve.boneName = boneName;
					transformCurve.begin = 0;

					auto animLayer = animStack->GetMember<FbxAnimLayer>(0);
					FbxAnimCurve* fbxTCurves[3];
					FbxAnimCurve* fbxRCurves[3];
					FbxAnimCurve* fbxSCurves[3];

					fbxTCurves[0] = boneNode->LclTranslation.GetCurve(animLayer, "X");

					if(!fbxTCurves[0])
						continue;
					
					fbxTCurves[1] = boneNode->LclTranslation.GetCurve(animLayer, "Y");
					fbxTCurves[2] = boneNode->LclTranslation.GetCurve(animLayer, "Z");

					fbxRCurves[0] = boneNode->LclRotation.GetCurve(animLayer, "X");
					fbxRCurves[1] = boneNode->LclRotation.GetCurve(animLayer, "Y");
					fbxRCurves[2] = boneNode->LclRotation.GetCurve(animLayer, "Z");

					fbxSCurves[0] = boneNode->LclScaling.GetCurve(animLayer, "X");
					fbxSCurves[1] = boneNode->LclScaling.GetCurve(animLayer, "Y");
					fbxSCurves[2] = boneNode->LclScaling.GetCurve(animLayer, "Z");

					// set & apply filter
					FbxAnimCurveFilterKeyReducer keyReducer;
					keyReducer.SetKeySync(false);
					keyReducer.Apply(fbxTCurves, 3);
					keyReducer.Apply(fbxSCurves, 3);
					keyReducer.SetKeySync(true);
					keyReducer.Apply(fbxRCurves, 3);

					FbxAnimCurveFilterUnroll unroll;
					unroll.SetForceAutoTangents(true);
					unroll.Apply(fbxRCurves, 3);

					FbxAnimCurveFilterTSS tss;
					FbxTime tt;
					tt.SetSecondDouble(-fbxTCurves[0]->KeyGetTime(0).GetSecondDouble());
					tss.SetShift(tt);
					tss.Apply(fbxTCurves, 3);
					tss.Apply(fbxRCurves, 3);
					tss.Apply(fbxSCurves, 3);
					//

					// process curves
					if(fbxTCurves[0]->KeyGetCount() > 0) {
						ProcessAnimCurveT(fbxTCurves, transformCurve);
						ProcessAnimCurveS(fbxSCurves, transformCurve);
						ProcessAnimCurveR(fbxRCurves, transformCurve, preRot);

						//clamping by reduced keyframes
						clip->startTime = 0;
						clip->lengthInSeconds = transformCurve.end;
						clip->lengthInFrames = (int)(1.5f + (transformCurve.end / (1 / 30.0f)));
					}
				} // animations loop
			} // cluster loop
		}

		float GameEngine::FbxLoader::FbxLoader::ProcessAnimCurve(FbxAnimCurve* curve, AnimCurve& animCurve)
		{
			if(!curve) return 0;

			int nkeys = curve->KeyGetCount();
			animCurve.keyframes.resize(nkeys);

			for(int i = 0; i < nkeys; ++i) {
				curve->KeySetTangentMode(i, FbxAnimCurveDef::eTangentAuto);
				auto& key = animCurve.keyframes[i];
				key.value = curve->KeyGetValue(i);
				key.leftTangent = curve->KeyGetLeftAuto(i);
				key.rightTangent = curve->KeyGetRightAuto(i);
				key.time = (float)curve->KeyGetTime(i).GetSecondDouble();
			}
			return (float)curve->KeyGetTime(nkeys - 1).GetSecondDouble();
		}

		void GameEngine::FbxLoader::FbxLoader::ProcessAnimCurveS(FbxAnimCurve* curve[3], AnimTransformCurve& animCurve)
		{
			if(!curve) return;

			auto& curve3 = animCurve.scaling;
			float endTime = 0;
			endTime = max(endTime, ProcessAnimCurve(curve[0], curve3.curves[0]));
			endTime = max(endTime, ProcessAnimCurve(curve[1], curve3.curves[1]));
			endTime = max(endTime, ProcessAnimCurve(curve[2], curve3.curves[2]));
			animCurve.end = max(animCurve.end, endTime);
		}

		void GameEngine::FbxLoader::FbxLoader::ProcessAnimCurveT(FbxAnimCurve* curve[3], AnimTransformCurve& animCurve)
		{
			if(!curve) return;

			auto& curve3 = animCurve.translation;
			float endTime = 0;
			endTime = max(endTime, ProcessAnimCurve(curve[0], curve3.curves[0]));
			endTime = max(endTime, ProcessAnimCurve(curve[1], curve3.curves[1]));
			endTime = max(endTime, ProcessAnimCurve(curve[2], curve3.curves[2]));

			auto& xcurve = curve3.curves[0];
			auto& ycurve = curve3.curves[1];
			auto& zcurve = curve3.curves[2];

			// apply scale factor
			for(int i = 0; i < xcurve.keyframes.size(); ++i) {
				auto& keyframe = xcurve.keyframes[i];
				if(axismode == eLeftHanded) {	
					keyframe.value *= -factor;
					keyframe.leftTangent *= -factor;
					keyframe.rightTangent *= -factor;
				}
				else {
					keyframe.value *= factor;
					keyframe.leftTangent *= factor;
					keyframe.rightTangent *= factor;
				}
			}
			for(int i = 0; i < ycurve.keyframes.size(); ++i) {
				auto& keyframe = ycurve.keyframes[i];
				keyframe.value *= factor;
				keyframe.leftTangent *= factor;
				keyframe.rightTangent *= factor;
			}
			for(int i = 0; i < zcurve.keyframes.size(); ++i) {
				auto& keyframe = zcurve.keyframes[i];
				keyframe.value *= factor;
				keyframe.leftTangent *= factor;
				keyframe.rightTangent *= factor;
			}

			animCurve.end = max(animCurve.end, endTime);
		}

		void GameEngine::FbxLoader::FbxLoader::ProcessAnimCurveR(FbxAnimCurve* curve[3], AnimTransformCurve& animCurve, FbxAMatrix & preRotation)
		{
			if(!curve) return;

			auto& curve3 = animCurve.rotation;

			int xKeys = curve[0]->KeyGetCount();
			int yKeys = curve[1]->KeyGetCount();
			int zKeys = curve[2]->KeyGetCount();

			// check key sync
			if(xKeys != yKeys || xKeys != zKeys)
				return;

			int nKeys = xKeys;

			curve3.curves[0].keyframes.resize(nKeys);
			curve3.curves[1].keyframes.resize(nKeys);
			curve3.curves[2].keyframes.resize(nKeys);

			animCurve.end = (float)curve[0]->KeyGetTime(nKeys - 1).GetSecondDouble();

			for(int ki = 0; ki < nKeys; ++ki) {
				auto& xkey = curve3.curves[0].keyframes[ki];
				auto& ykey = curve3.curves[1].keyframes[ki];
				auto& zkey = curve3.curves[2].keyframes[ki];

				curve[0]->KeySetTangentMode(ki, FbxAnimCurveDef::eTangentAuto);
				curve[1]->KeySetTangentMode(ki, FbxAnimCurveDef::eTangentAuto);
				curve[2]->KeySetTangentMode(ki, FbxAnimCurveDef::eTangentAuto);
				auto xvalue = curve[0]->KeyGetValue(ki);
				auto yvalue = curve[1]->KeyGetValue(ki);
				auto zvalue = curve[2]->KeyGetValue(ki);

				auto xlt = curve[0]->KeyGetLeftAuto(ki);
				auto xrt = curve[0]->KeyGetRightAuto(ki);
				auto ylt = curve[1]->KeyGetLeftAuto(ki);
				auto yrt = curve[1]->KeyGetRightAuto(ki);
				auto zlt = curve[2]->KeyGetLeftAuto(ki);
				auto zrt = curve[2]->KeyGetRightAuto(ki);

				FbxAMatrix temp;
				temp.SetR(FbxVector4(xvalue, yvalue, zvalue));
				auto R = (preRotation* temp).GetR();
				xvalue = (float)R[0]; yvalue = (float)R[1]; zvalue = (float)R[2];

				float t = (float)curve[0]->KeyGetTime(ki).GetSecondDouble();

				if(axismode == eLeftHanded) {
					yvalue *= -1; ylt *= -1; yrt *= -1;
					zvalue *= -1; zlt *= -1; zrt *= -1;
				}

				xkey.time = t; ykey.time = t; zkey.time = t;
				xkey.value = xvalue; ykey.value = yvalue; zkey.value = zvalue;
				xkey.leftTangent = xlt; xkey.rightTangent = xrt;
				ykey.leftTangent = ylt; ykey.rightTangent = yrt;
				zkey.leftTangent = zlt; zkey.rightTangent = zrt;
			}
		}

		void FbxLoader::ComputeNodeMatrix(FbxNode* node, Node& meshNode, bool local)
		{
			if(!node)
				return;
			FbxAnimEvaluator* evaluator = scene->GetAnimationEvaluator();
			FbxAMatrix global;
			global.SetIdentity();

			FbxTime time;
			time.SetSecondDouble(0.0);
			
			if(node != scene->GetRootNode()) {
				if(local) {
					global = evaluator->GetNodeLocalTransform(node, time);
				}
				else {
					global = evaluator->GetNodeGlobalTransform(node, time);
				}
			}
			auto T = global.GetT() * factor;
			
			if(axismode == eLeftHanded) {
				auto R = global.GetR();
				R[1] *= -1; R[2] *= -1;
				T[0] *= -1;
				global.SetR(R);
			}
			global.SetT(T);

			meshNode.matrix = Matrix(
				(float)global[0][0], (float)global[0][1], (float)global[0][2], (float)global[0][3],
				(float)global[1][0], (float)global[1][1], (float)global[1][2], (float)global[1][3],
				(float)global[2][0], (float)global[2][1], (float)global[2][2], (float)global[2][3],
				(float)global[3][0], (float)global[3][1], (float)global[3][2], (float)global[3][3]);
		}

		void FbxLoader::LoadAnimationClipData()
		{
			// load animation data
			const int animCount = scene->GetSrcObjectCount<FbxAnimStack>();

			for(int ac = 0; ac < animCount; ++ac) {
				FbxAnimStack* currAnimStack = scene->GetSrcObject<FbxAnimStack>(ac);
				if(!currAnimStack) return;

				FbxString animStackName = currAnimStack->GetName();
				FbxTakeInfo* takeInfo = scene->GetTakeInfo(animStackName);

				if(!takeInfo) return;
				FbxTime start = takeInfo->mLocalTimeSpan.GetStart();
				FbxTime stop = takeInfo->mLocalTimeSpan.GetStop();
				FbxTime::EMode mode = FbxTime::eDefaultMode;
				int startFrame = (int)start.GetFrameCount(mode);
				int stopFrame = (int)stop.GetFrameCount(mode);
				int animLength = stopFrame - startFrame + 1;

				shared_ptr<AnimClip> clip = make_shared<AnimClip>();
				clip->name = animStackName;
				animationClips[clip->name] = clip;
			}
		}


		void FbxLoader::ConvertMatrix(Matrix& dest, FbxAMatrix& src)
		{
#pragma omp parallel for
			for(int i = 0; i < 4; i++) {
				for(int j = 0; j < 4; j++)
					dest.m[i][j] = (float)src[i][j];
			}
		}
	}
}